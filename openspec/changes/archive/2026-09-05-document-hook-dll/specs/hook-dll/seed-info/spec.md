## Purpose

Covers on-demand item stat generation: the client asks "what would this item's tooltip say", and the hook answers by briefly creating the item inside the running game, rendering its display text, destroying it, and writing the result back as JSON. This is what lets the client show real, game-computed stats for items it only holds as seeds and record names.

## ADDED Requirements

### Requirement: Accept stat requests as files, one request per file

The hook SHALL poll a request folder belonging to the current world and read each request file it finds. A request identifies the item either by a numeric item identifier or by an opaque identifier belonging to another player's collection, followed by the seeds and record names needed to reconstruct it.

Request reading SHALL happen on a background thread so that no file input or output occurs inside the game loop. Every file encountered SHALL be consumed — deleted — whether or not it parsed, so a malformed request cannot be retried forever.

Files that are not requests SHALL be logged and consumed. A request whose field count matches no known format SHALL be rejected with a logged warning naming the count found.

When no world is loaded, the poll SHALL be skipped rather than defaulting to a folder.

Unlike the deposit queue, the request folder SHALL be qualified by mod name only and NOT by the hardcore flag — stat generation does not depend on which collection the item belongs to. Both sides must agree on this, so it is stated here rather than left to be inferred from the deposit path's shape.

#### Scenario: Request folder for a modded world

- **WHEN** requests are polled for a world running a mod
- **THEN** the folder is qualified by the mod name alone, and the same folder is used whether or not the world is hardcore

#### Scenario: Valid request appears

- **WHEN** the client writes a stat request for the current world
- **THEN** it is parsed, queued for the game thread, and removed from the request folder

#### Scenario: Malformed request

- **WHEN** a request file cannot be parsed
- **THEN** it is logged, removed, and no work is queued for it

### Requirement: Generate stats on the game's render thread, only while the world is alive

Item creation and tooltip rendering SHALL happen on the game's own thread, driven from the render path, and only while a live world exists. Creating an item with a set bonus from the main menu can crash the game, and items with skills render incomplete information there.

The hook SHALL re-check that the world is alive between items rather than once per batch, and SHALL bound how many requests it processes in a single frame so that a large backlog does not stall rendering.

If the hook cannot take its lock immediately it SHALL skip the frame rather than block the render thread.

#### Scenario: Request arrives at the main menu

- **WHEN** stat requests are pending but no world is alive
- **THEN** nothing is generated, the reason is logged, and the hook backs off before trying again

#### Scenario: Large backlog

- **WHEN** many requests are pending at once
- **THEN** only a bounded number are processed per frame and the rest are carried to later frames

#### Scenario: Lock contended

- **WHEN** another thread holds the lock when the render frame runs
- **THEN** the frame does its normal work without generating stats

### Requirement: Back off after startup and after an unavailable world

The hook SHALL delay its first generation pass for several seconds after the game loads, and SHALL back off for a longer interval whenever it finds no live world, because generating aggressively as the game loads tends to crash it.

The back-off SHALL be interruptible so that shutdown is not delayed by a pending wait.

#### Scenario: Game just loaded

- **WHEN** the hook starts up
- **THEN** it waits several seconds before its first generation pass

#### Scenario: Shutdown during back-off

- **WHEN** the hook is asked to stop while waiting out a back-off
- **THEN** the wait is abandoned promptly and the thread exits

### Requirement: Render each item, then destroy it

For each request the hook SHALL create a throwaway item from the supplied replica, render its display text against the current player character including set-bonus details, and then destroy the item through the game's own object manager. A created item MUST NOT be left in the game.

Relics SHALL be rendered through the display-text entry point for their own item class rather than the equipment one.

The caller is responsible for only requesting items this path supports: equipment that is not part of a set, and not a potion, scroll, or other special item.

#### Scenario: Item rendered

- **WHEN** an item is created successfully and a player character exists
- **THEN** its display text is rendered, the item is destroyed, and the result is recorded

#### Scenario: Game cannot create the item

- **WHEN** the game refuses to create an item from the request's replica
- **THEN** a "no item" error message carrying the item's base record is sent to the client and no result is recorded

#### Scenario: No engine or no character

- **WHEN** no game engine or no player character is available
- **THEN** a "no game" error message is sent to the client and generation is skipped for that request

### Requirement: Publish results as one JSON file per batch

Results SHALL be written to a results folder as a single JSON document keyed by request identifier. Each entry SHALL carry the requesting identifiers, an echo of the replica the stats were computed from, and the rendered stat lines with their text classes.

The file SHALL be published atomically — written under a name the client ignores and renamed to the name it polls for — so the client never reads a partially written result.

Nothing SHALL be written when a batch produced no results.

#### Scenario: Batch produces results

- **WHEN** one or more requests in a frame produce stats
- **THEN** a single uniquely named JSON file containing all of them is published, and its publication is logged

#### Scenario: Batch produces nothing

- **WHEN** every request in a frame failed or the queue was empty
- **THEN** no result file is created
