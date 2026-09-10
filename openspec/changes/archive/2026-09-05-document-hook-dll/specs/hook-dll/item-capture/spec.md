## Purpose

Covers "instaloot": intercepting an item as the player moves it into a designated stash tab, deciding whether Item Assistant can handle that item, and handing it off to the client as a file on disk instead of letting it land in the stash.

## ADDED Requirements

### Requirement: Capture items placed into the configured loot tab only

The hook SHALL intercept items being added to an inventory sack, and SHALL act only when the destination sack is the transfer-stash tab configured as the loot tab. Items moved anywhere else — the player's own inventory, other tabs, the private stash — SHALL pass through untouched.

The loot tab SHALL be read from the client's settings, where an unset or zero value means "the last transfer tab". A configured tab number SHALL be clamped to the range of tabs that actually exist. Capture SHALL be disabled entirely when fewer than two transfer tabs exist.

Both ways the game adds an item to a sack — placement at a computed position, and placement at an explicit position — SHALL be intercepted.

#### Scenario: Item moved into the loot tab

- **WHEN** the player moves a supported item into the configured loot tab while capture is active and a world is alive
- **THEN** the item is written out for the client, the game is told the add succeeded, and the item does not appear in the stash

#### Scenario: Item moved elsewhere

- **WHEN** the player moves an item into any sack that is not the configured loot tab
- **THEN** the hook does not act and the game's own behaviour is unchanged

#### Scenario: Client is not running

- **WHEN** capture is inactive because the client is closed
- **THEN** items move into the loot tab normally

### Requirement: Refuse item categories Item Assistant does not handle, and say why

Before capturing, the hook SHALL classify the item and decline anything it cannot represent. It SHALL decline: stacked items, components, crafting materials, quest items, and the miscellaneous and special items that are known not to round-trip — with a small explicit allow-list of story items that are supported.

Every refusal SHALL show the player an in-game message naming the reason, so a non-captured item is never silently left in the tab without explanation.

Capture SHALL also be refused, with a message, while the client reports that the game's data has not been parsed yet; the hook SHALL re-check that setting rather than caching a stale "not parsed".

#### Scenario: Player moves a stack of components

- **WHEN** a stackable item is moved into the loot tab
- **THEN** it is not captured, it lands in the tab normally, and the player sees a message saying stackable items are not looted

#### Scenario: Player moves an allow-listed story item

- **WHEN** one of the explicitly supported story items is moved into the loot tab
- **THEN** it is captured like any ordinary equipment item

#### Scenario: Game data not yet parsed

- **WHEN** an item is moved into the loot tab before the client has parsed the game's data
- **THEN** the item is not captured and the player is told why; once parsing completes, capture resumes and the player is told monitoring is enabled

### Requirement: Persist a captured item as a self-describing file

A captured item SHALL be written to the client's incoming queue folder as a single file containing the item's replica data plus, when available, the stat lines the game would show in its tooltip.

The file SHALL be encoded in UTF-8 so that characters outside the machine's local codepage survive, SHALL carry the world's mod name and hardcore flag alongside the replica so the client knows which collection it belongs to, and SHALL sanitise text so that no captured value can break the file's line structure.

The file SHALL be published atomically — written under a temporary name and renamed once complete — so the client never reads a partial capture. Its name SHALL be unique.

The item SHALL be reported as captured to the game only if the file was written successfully; otherwise the item SHALL fall through to the game's normal handling.

#### Scenario: Capture with stat lines

- **WHEN** an item is captured while a player character exists
- **THEN** the file contains the replica line followed by the item's rendered stat lines, each tagged with its text class

#### Scenario: Capture without a character

- **WHEN** an item is captured but no player character is available to render stats against
- **THEN** the item is still captured, without stat lines, and the omission is logged

#### Scenario: Write fails

- **WHEN** the capture file cannot be written or published
- **THEN** the failure is logged and the item is added to the stash by the game as normal

### Requirement: Confirm a capture to the player

On a successful capture the hook SHALL show an in-game confirmation and play the game's own item-drop sound, so the item visibly and audibly goes somewhere rather than appearing to vanish.

In-game messages SHALL be rate-limited to roughly one per three seconds — about the time one takes to fade — so that bulk transfers do not flood the screen. Suppressed messages SHALL still be logged.

#### Scenario: Bulk transfer

- **WHEN** the player moves many items into the loot tab in quick succession
- **THEN** every item is captured, but at most one message per rate-limit window is displayed

#### Scenario: No engine available to display a message

- **WHEN** the hook wants to show a message but the game engine is unavailable
- **THEN** the message is skipped and logged, and capture is otherwise unaffected
