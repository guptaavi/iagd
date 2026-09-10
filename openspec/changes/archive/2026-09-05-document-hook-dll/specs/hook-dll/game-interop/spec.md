## Purpose

Defines the rules for reaching into a shipped, unmodified Grim Dawn: how the hook locates game functions and data, how it mirrors game structures that the game itself writes into, and how it decides whether there is a live world safe to touch. These are the constraints that let the hook survive a game patch instead of crashing the player.

## ADDED Requirements

### Requirement: Resolve game functions by exported symbol, and degrade when one is missing

The hook SHALL reach every game function and global through the game modules' exported symbols rather than hard-coded addresses, so that a rebuilt game binary relocates transparently.

A symbol that cannot be resolved SHALL be treated as a recoverable condition — almost always a game patch that changed a signature — not as a fatal one. The hook SHALL log the missing symbol, report the failure to the client, and SHALL NOT install a hook or call through a null pointer.

Successful resolutions SHALL be logged once per symbol; symbols looked up every frame MUST NOT produce a log line per lookup.

#### Scenario: A patched game renames a function

- **WHEN** a game update changes a hooked function's signature, so its exported symbol no longer exists
- **THEN** the affected hook is skipped, a hook-error message identifying it is sent to the client, and the remaining hooks still install

#### Scenario: A global pointer export is unavailable

- **WHEN** the hook looks up a game global whose export is not present, for example because the game module is not loaded yet
- **THEN** the lookup returns "not available" and the hook MUST NOT dereference the missing export

#### Scenario: Repeated lookups of the same symbol

- **WHEN** the same symbol is resolved on every game tick
- **THEN** at most one success line for that symbol appears in the log

### Requirement: Mirror the game's item replica layout exactly

The hook exchanges an item-replica structure with the game: the game writes it when the hook asks for an item's replica data, and reads it back when the hook asks the game to create an item. Its layout MUST match the shipped game binary byte for byte, including fields whose purpose is unknown and which exist only to make the size correct. A layout that is too small causes the game to write past the end of the hook's object, which under the capture path is a stack local.

The declared size SHALL be enforced at build time, so that a layout change cannot ship silently. Any change to the layout SHALL be verified against the assignment operator in the target game binary first.

#### Scenario: Layout no longer matches the game

- **WHEN** the mirrored structure's size differs from the size the target game build uses
- **THEN** the build fails rather than producing a DLL that corrupts the stack at runtime

#### Scenario: Constructing a replica for the game to read

- **WHEN** the hook builds a replica from data supplied by the client rather than from the game
- **THEN** every field is initialised, and the stack-size field defaults to 1 to match the game's own constructor — a stack size of 0 produces items the game does not count as crafting ingredients

### Requirement: Determine whether a live world exists before touching it

The hook SHALL expose a single "is there a world safe to touch" check and use it to gate all work that calls into the game. The individual game state flags are weaker than their names suggest and none of them go false when the world is torn down on exit-to-menu, so the check SHALL additionally require that a main player object exists.

The check SHALL be re-evaluated between units of work rather than once up front, because the answer can change mid-loop.

#### Scenario: Player exits to the main menu mid-loop

- **WHEN** the world is torn down while the hook is processing a batch of queued work
- **THEN** the check reports no live world on the next iteration and the remaining work is deferred rather than executed against a dead world

#### Scenario: Game state flags disagree with reality

- **WHEN** the game's own loading and online flags both report a healthy engine but no main player exists
- **THEN** the hook treats the world as not alive

### Requirement: Resolve the current world's mod name and hardcore flag on the game thread only

The mod name and hardcore flag identify which queue folder every other capability reads from and writes to; guessing them means operating on another character's queue. The hook SHALL read them out of the game only from the game's own thread, cache them per world, and expose a thread-safe read-only copy to background threads.

Background threads SHALL skip their work entirely when no world is currently known, rather than falling back to a default. The cache SHALL be invalidated when the world is torn down.

The Crucible game mode SHALL NOT be treated as a mod, so the mod name is empty there.

#### Scenario: Background thread runs before a world is loaded

- **WHEN** a polling thread asks for the current world context and no world has been resolved
- **THEN** it is told there is no context and skips this round without reading any queue folder

#### Scenario: World changes

- **WHEN** the player loads a different world
- **THEN** the next resolution on the game thread reads fresh values, caches them, and logs the resolved mod name and hardcore flag once

#### Scenario: World is torn down

- **WHEN** the world's game info object disappears while the engine is still ticking
- **THEN** the cached context is invalidated so no background thread keeps working the departing world's folder

### Requirement: Render item tooltips through the item's own virtual dispatch

Rendering an item's stat lines requires calling the game's display-text method for that item's concrete type. The hook SHALL determine the virtual table slot of that method once, by locating the method's address inside an exported virtual table, trying each known item class in turn until one resolves. It SHALL then call through the slot of the object it was given, so that any item type dispatches correctly.

If no candidate class resolves, the hook SHALL disable this facility rather than guess a slot, and item capture SHALL continue without stat lines.

#### Scenario: Slot resolution succeeds

- **WHEN** at least one known item class exposes both its virtual table and its display-text method as exports
- **THEN** the slot index is resolved once, logged, and reused for every subsequent call

#### Scenario: Slot resolution fails after a game patch

- **WHEN** no candidate class can be resolved
- **THEN** the facility is permanently disabled for this session, the failure is logged, and callers receive no stat lines instead of calling an arbitrary virtual slot
