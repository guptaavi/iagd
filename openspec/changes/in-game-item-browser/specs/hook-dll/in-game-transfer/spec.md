## Purpose

Covers the path from "the player picked an item in the overlay" to "the item is in the transfer stash and the client knows it is gone": creating the item in-process under the same safety conditions the existing deposit path uses, and handing the resulting removal back to the client durably enough to survive the game closing at the wrong moment.

## ADDED Requirements

### Requirement: Create the item under the same conditions as a client-driven deposit

An item requested from the overlay SHALL be created and placed into the transfer stash on the game's own update thread, only while a live world exists and the transfer stash is open, and into the same destination sack a client-driven deposit would use.

The overlay MAY accept the request at any time it is open, but the request SHALL be held until those conditions hold rather than executed against a game that cannot receive it.

If the destination sack is full, or the item cannot be created — for example a modded item requested into a vanilla world — the item SHALL NOT be recorded as transferred, and the player SHALL be told why.

#### Scenario: Transfer stash is open

- **WHEN** the player transfers an item from the overlay while the transfer stash is open in a live world
- **THEN** the item is created and placed into the destination sack on the game's update thread

#### Scenario: Transfer stash is closed

- **WHEN** the player transfers an item from the overlay while the transfer stash is closed
- **THEN** the item is not created, the request is held or declined, and the player is told that the transfer stash must be open

#### Scenario: Destination sack is full

- **WHEN** an item cannot be placed because the destination sack has no room
- **THEN** the item is not recorded as transferred, it remains available in the client, and the player is told the stash is full

#### Scenario: Item cannot be created

- **WHEN** the game refuses to create the requested item
- **THEN** nothing is recorded as transferred, the failure is logged, and the player is told the item could not be transferred

### Requirement: Record every completed transfer durably, for the client to act on

Once an item has actually been placed in the stash, the DLL SHALL record that fact in a durable, on-disk record that the client consumes. The DLL MUST NOT remove the item from the database itself.

The record SHALL identify the item precisely enough for the client to remove exactly that item and perform its own bookkeeping, and SHALL carry an identifier unique to that transfer.

The record SHALL be published atomically, so that the client never reads a partially written one.

A record SHALL be written only after the item is confirmed placed. A transfer that is recorded but never happened costs the player an item; a transfer that happened but was not recorded is recoverable by the player. The ordering deliberately favours the second.

#### Scenario: Item placed successfully

- **WHEN** an item is placed into the transfer stash from the overlay
- **THEN** a durable record identifying that item and that transfer is published for the client

#### Scenario: Game closes immediately after placing an item

- **WHEN** the game exits after the record has been published but before the client has consumed it
- **THEN** the record survives and the client acts on it when it next runs

#### Scenario: Placement fails

- **WHEN** placing the item fails for any reason
- **THEN** no record is published

### Requirement: Let the client apply a transfer exactly once

The client SHALL apply each recorded transfer at most once, however many times it reads the record, and SHALL discard the record only after the removal has been applied.

Reading the same record twice must not decrement a stack twice. The client SHALL use the transfer's identifier to recognise work it has already done.

The client SHALL process outstanding records when it starts, before any state that depends on the item collection is published elsewhere.

#### Scenario: Client consumes a record

- **WHEN** the client reads a record for an item that was placed in the stash
- **THEN** it removes that item from its collection, performs its usual bookkeeping for a removed item, and discards the record

#### Scenario: Client restarts mid-consume

- **WHEN** the client stops after applying a removal but before discarding the record
- **THEN** re-reading the record on the next start does not remove anything a second time

#### Scenario: Records outstanding at startup

- **WHEN** the client starts with records left over from a previous session
- **THEN** it applies them before it publishes its item collection anywhere

### Requirement: Leave the existing client-driven deposit path unchanged

The queue-folder deposit path the client uses today SHALL continue to work exactly as before, with the same folders, the same file handling, and the same behaviour when the DLL and client versions differ.

A player SHALL be able to transfer from the client and from the overlay in the same session without either path interfering with the other.

#### Scenario: Transfer from the client

- **WHEN** the player transfers an item from the client while the overlay exists
- **THEN** it is deposited through the existing queue-folder path with no change in behaviour

#### Scenario: Both paths in one session

- **WHEN** the player transfers items from both the client and the overlay in one session
- **THEN** both arrive in the stash and each item is removed from the collection exactly once
