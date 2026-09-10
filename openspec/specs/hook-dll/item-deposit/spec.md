## Purpose

Covers the return path: items the player sends back from Item Assistant are picked up from a queue folder on disk, recreated inside the game, and placed into the transfer stash — with every game call made on the game's own thread and only while the stash is actually open.

## Requirements

### Requirement: Discover pending deposits on a background thread

The hook SHALL poll the client's outgoing queue folder for the current world and collect newly seen deposit files into a pending set shared with the game thread. Polling SHALL be done off the game thread so that no directory listing happens inside the game loop.

The folder polled SHALL be selected by the current world's mod name and hardcore flag. When no world is known the poll SHALL be skipped entirely rather than defaulting to a folder, because guessing means reading another character's queue.

Only files of the expected type SHALL be collected; anything else SHALL be logged and ignored. A file already seen SHALL NOT be collected twice.

The polling thread SHALL start when capture becomes active and stop when it becomes inactive.

#### Scenario: No world loaded

- **WHEN** the polling thread runs while no world is loaded
- **THEN** it skips the round without listing any folder

#### Scenario: New deposit file appears

- **WHEN** the client writes a deposit file for the current world
- **THEN** the file is added to the pending set within roughly half a second, and is not added again on later polls

### Requirement: Deposit only on the game thread, and only with the transfer stash open

Recreating an item and placing it into a sack SHALL happen on the game's own update thread, only while the transfer stash is open, and only while a live world exists. The hook SHALL track the stash's open state from the game and SHALL treat the stash as closed whenever the world is not alive.

To bound the cost, the hook SHALL do this work on a fraction of update ticks rather than every tick. Access to the pending set SHALL be mutually exclusive with the polling thread.

When the game's world information disappears mid-teardown, the hook SHALL skip its work for that tick, invalidate the cached world context, and still let the game's own update run — a dropped update tick is a visible stall.

#### Scenario: Stash is closed

- **WHEN** deposits are pending but the transfer stash is not open
- **THEN** nothing is deposited and the files stay pending

#### Scenario: World tears down during deposits

- **WHEN** the world's game information disappears while an update tick is being processed
- **THEN** the hook skips its own work, invalidates the cached world context, logs the condition, and still calls through to the game's update

### Requirement: Place each deposited item, and return anything that cannot be placed

For each pending file the hook SHALL reconstruct the item's replica, ask the game to create the item, find a free position in the configured deposit tab, and place it there.

The deposit tab SHALL be read from the client's settings, where an unset or zero value means "the second-to-last transfer tab", and a configured tab number SHALL be clamped to the tabs that exist. Deposits SHALL be disabled when fewer than two transfer tabs exist.

An item that cannot be recreated — for example a mod item carried into a vanilla game — or that cannot be placed because the tab is full SHALL be returned to the client's incoming queue rather than lost. Every consumed file SHALL be moved out of the outgoing folder exactly once, whether it succeeded or not.

After a successful batch the hook SHALL sort the destination sack, because items are placed one by one from the same starting position.

#### Scenario: Destination tab is full

- **WHEN** an item is recreated but no free position exists in the deposit tab
- **THEN** the item is not placed, its file is moved back into the incoming queue, and the player is told the item was moved back to the client

#### Scenario: Item cannot be recreated

- **WHEN** the game refuses to create an item from a deposit file's replica
- **THEN** the failure is logged, the file is moved back into the incoming queue, and the remaining deposits still proceed

#### Scenario: Successful batch

- **WHEN** one or more items are placed successfully
- **THEN** the player sees a single message stating how many items were deposited, and the destination sack is sorted

### Requirement: Soft-delete consumed deposit files

A deposit file that has been acted on SHALL be moved into a per-world "deleted" folder rather than being deleted outright, leaving the client responsible for removing them in its own time. This preserves a record if a deposit is later found to have gone wrong.

Files SHALL be given a fresh unique name on the move so that two deposits cannot collide.

#### Scenario: File consumed

- **WHEN** a deposit file has been processed, successfully or not
- **THEN** it no longer appears in the outgoing folder and appears exactly once in the corresponding deleted folder or back in the incoming queue

#### Scenario: Move fails

- **WHEN** the file cannot be moved
- **THEN** the failure and the operating system's error code are logged, and the rest of the batch continues

### Requirement: Read historical deposit file formats

Deposit files have gained columns over time. The hook SHALL accept every format version the client may still have on disk, distinguishing them by column count, and SHALL reject a file whose column count matches no known version rather than misreading it.

Columns absent from an older format SHALL take their documented defaults.

#### Scenario: Legacy file without the newer columns

- **WHEN** a deposit file written by an older client is read
- **THEN** the known columns are parsed, the missing ones take their defaults, and the item deposits normally

#### Scenario: Unrecognised file

- **WHEN** a deposit file has a column count matching no known format
- **THEN** it is rejected with a logged warning naming the count found, and no item is created
