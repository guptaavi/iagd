## Purpose

Covers turning a stored item into something a player can recognise at a glance: its icon and quality, its stat text as the game itself renders it including colour and section structure, and the collapsing of identical items into a single stack.

## ADDED Requirements

### Requirement: Show each item with its icon and quality

Every item in the result SHALL be shown with its name, its icon, and a visual indication of its quality.

The icon SHALL be resolved from the item's records using the same precedence the client uses, so the overlay and the client show the same icon for the same item. An item whose icon cannot be resolved SHALL fall back to a placeholder rather than showing an empty space or failing the result.

Icons SHALL be read from the images the client has already extracted; the overlay MUST NOT extract them from the game's own asset archives.

#### Scenario: Item with a resolvable icon

- **WHEN** an item is shown whose records name an icon the client has extracted
- **THEN** that icon is displayed, matching what the client shows for the same item

#### Scenario: Icon is missing

- **WHEN** an item's icon cannot be resolved or its image file is absent
- **THEN** a placeholder is shown and the rest of the item renders normally

#### Scenario: Many items on screen

- **WHEN** a page of results is displayed
- **THEN** icons are loaded and reused without reloading the same image for every item that uses it

### Requirement: Render stored stat text with its colours and structure

For an item that has stored stat text, the overlay SHALL render those rows in order, honouring the colour markers embedded in the text and the row type that identifies each row's role.

Rows SHALL be visually distinguished by role — separators, requirements, granted skills, set information and ordinary stat lines are not interchangeable — so the result reads like the game's own tooltip rather than a flat list of strings.

A colour marker the overlay does not recognise SHALL be dropped rather than displayed as literal text.

Stored set-bonus rows SHALL be handled so that lines belonging to a set bonus are not presented as though they were the item's own stats.

#### Scenario: Item with colour-marked stat text

- **WHEN** an item's stored stat rows contain colour markers
- **THEN** the text is displayed coloured and the markers themselves are not visible

#### Scenario: Item with granted skills and set information

- **WHEN** an item's stored rows include granted skills and set information
- **THEN** those sections are visually distinct from the item's ordinary stat lines

#### Scenario: Item has no stored stat text

- **WHEN** an item has no stored stat rows
- **THEN** its name, icon and quality are still shown, and the stat area indicates that details are unavailable rather than appearing broken

#### Scenario: Unrecognised marker

- **WHEN** a stored row contains a colour marker the overlay does not know
- **THEN** the marker is dropped and the surrounding text is still readable

### Requirement: Merge identical items into one entry with a count

Items that are identical for the player's purposes SHALL be presented as a single entry carrying a count, rather than as repeated entries.

Merging SHALL use the same identity the client uses, so that a stack in the overlay corresponds to a stack in the client.

A merged entry SHALL allow the player to act on one item from the stack without acting on all of them.

#### Scenario: Several copies of the same item

- **WHEN** a search matches several items that are identical
- **THEN** they appear as one entry showing how many there are

#### Scenario: Taking one from a stack

- **WHEN** the player transfers from a merged entry
- **THEN** one item is transferred and the entry's count decreases, leaving the rest in place
