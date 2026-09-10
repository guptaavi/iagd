## Purpose

Covers how the overlay finds items: reading the Item Assistant client's item database from inside the game process without interfering with the client that owns it, the filters a player can apply, and how results are paged and kept current while the client writes underneath.

## ADDED Requirements

### Requirement: Read the client's database without writing to it

The DLL SHALL open the client's item database read-only and MUST NOT write to it under any circumstance, including recovery paths. The client remains the sole writer.

The DLL SHALL locate the database through the same client storage folder it already uses for its other files, rather than through a separate configured path that could drift.

The DLL SHALL tolerate the database being absent, unreadable, or of an unexpected shape: the overlay reports that no items are available and stays usable, rather than failing the hook or the game.

#### Scenario: Database is present

- **WHEN** the overlay opens and the client's database is present
- **THEN** it is opened read-only and searches run against it

#### Scenario: Database is missing or unreadable

- **WHEN** the client's database cannot be opened
- **THEN** the overlay reports that no items are available, logs the reason, and neither the hook nor the game is affected

#### Scenario: Client writes while the overlay reads

- **WHEN** the client writes to the database while the overlay has it open
- **THEN** neither side is blocked into failure, and the overlay's reads either see the state before the write or after it, never a partial one

### Requirement: Never query from a game thread

A search SHALL be executed off the game's threads. A game thread MAY request a search and MAY consume a completed result, but MUST NOT wait for a query to finish.

The item database is large enough that a search cannot be bounded to a frame's budget, and a query on the presentation thread stalls the game visibly.

#### Scenario: Player types in the search field

- **WHEN** a search is requested from the overlay
- **THEN** it is handed to a worker, the frame completes without waiting for it, and the results appear in a later frame

#### Scenario: Search is superseded

- **WHEN** the player changes the search before the previous one has finished
- **THEN** the stale result is discarded rather than displayed, and the frame rate is not affected by the abandoned work

### Requirement: Match the client's search semantics for the filters it supports

For any filter the overlay offers, a search SHALL return the same set of items the client returns for the equivalent query against the same database. The overlay MAY support a subset of the client's filters; it MUST NOT interpret a shared filter differently.

The overlay SHALL support at minimum: free-text matching over item names and stat text, minimum and maximum level, item quality, equipment slot, and the presence of chosen item stats.

The overlay SHALL scope every search to the world the player is currently in — its mod and its hardcore flag — taken from the game rather than chosen by the player. A player in a world cannot usefully be shown items that cannot be transferred into it.

#### Scenario: Same filters, same results

- **WHEN** the overlay and the client run equivalent queries against the same database
- **THEN** they return the same items

#### Scenario: Hardcore character

- **WHEN** the player opens the overlay on a hardcore character
- **THEN** only hardcore items for the current mod are searched, with no way to select otherwise

#### Scenario: No world loaded

- **WHEN** a search would run with no world loaded
- **THEN** it does not run, because the mod and hardcore flag it must be scoped by are unknown

### Requirement: Bound result size and page beyond it

A search SHALL return at most a bounded number of items in one pass, and SHALL be able to fetch further pages on demand in a stable order.

Paging SHALL be ordered deterministically so that no item is skipped or repeated between pages.

When more items match than were returned, the overlay SHALL indicate that the result was capped rather than presenting it as complete.

#### Scenario: More matches than the cap

- **WHEN** a search matches more items than one pass returns
- **THEN** the first page is shown, the result is marked as capped, and further pages can be fetched

#### Scenario: Paging through results

- **WHEN** successive pages are fetched for one search
- **THEN** every matching item appears exactly once across the pages

### Requirement: Notice when the client changes the data

The overlay SHALL detect that the database has changed since its current results were produced, and SHALL offer the player up-to-date results rather than silently showing a stale list.

An item transferred or deleted through the client, or newly looted, changes what the overlay should show. The overlay MUST NOT let a player act on an item that no longer exists.

Detection SHALL be cheap enough to run while the overlay is open without measurable cost to the game.

#### Scenario: Client transfers an item while the overlay is open

- **WHEN** the client removes an item that the overlay is currently displaying
- **THEN** the overlay detects the change and refreshes rather than continuing to offer that item

#### Scenario: Nothing has changed

- **WHEN** the overlay checks for changes and the database has not been written
- **THEN** no query runs and no results are rebuilt
