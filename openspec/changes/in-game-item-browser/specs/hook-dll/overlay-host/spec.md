## Purpose

Governs the overlay's foothold inside the game process: obtaining a render target within the game's own frame, taking mouse and keyboard away from the game while the overlay is open, and the conditions under which the overlay offers itself at all. Nothing here concerns what the overlay shows — only that it can be drawn and driven without disturbing the game.

## ADDED Requirements

### Requirement: Draw only inside the game's own frame, on the game's own thread

The overlay SHALL be drawn from within the game's frame-presentation path, on the thread the game presents on, and only after the game has finished drawing its own content for that frame. It MUST NOT present, resize, or otherwise take ownership of the swap chain.

The overlay SHALL restore the graphics pipeline state it found to exactly what it was before it drew. The game does not expect its render state to change underneath it, and a leaked state binding corrupts the next frame rather than the overlay's own.

Drawing SHALL be skipped entirely for any frame in which the overlay is closed, so that a closed overlay costs the game nothing beyond the hook itself.

#### Scenario: Overlay is open

- **WHEN** the game finishes a frame while the overlay is open
- **THEN** the overlay is drawn on top of the game's content, the pipeline state is restored, and the game's own presentation proceeds unchanged

#### Scenario: Overlay is closed

- **WHEN** the game finishes a frame while the overlay is closed
- **THEN** nothing is drawn and no graphics state is read or written

#### Scenario: Drawing raises an error

- **WHEN** an error escapes while the overlay is being drawn
- **THEN** it is caught and logged, the overlay is closed rather than retried every frame, and the game's own presentation still runs

### Requirement: Available only on the renderer the overlay supports

The overlay SHALL make itself available only when the game is running a renderer it can draw into. On any other renderer it SHALL disable itself, record why in the hook log, and leave every other hook behaving exactly as it does without this change.

A player on an unsupported renderer SHALL still be told, once, that the in-game browser is unavailable and why, rather than being left to wonder why the hotkey does nothing.

#### Scenario: Supported renderer

- **WHEN** the game is running the renderer the overlay supports
- **THEN** the overlay's hooks are installed and the overlay can be opened

#### Scenario: Unsupported renderer

- **WHEN** the game is running a renderer the overlay cannot draw into
- **THEN** the overlay's hooks are not installed, the reason is logged, item capture and deposit continue to work, and the player is told once that the in-game browser is unavailable on this renderer

### Requirement: Take input from the game while the overlay is open

While the overlay is open, the mouse and keyboard events the game would have consumed for that frame SHALL be routed to the overlay and SHALL NOT reach the game. The game SHALL observe an empty input queue and no held buttons, rather than observing events it is asked to ignore.

While the overlay is closed, input SHALL reach the game exactly as it does without this change; the overlay MUST NOT alter, reorder, or delay it.

Suppression SHALL cover held-button state as well as queued events, so that a button pressed before the overlay opened does not remain held from the game's point of view.

#### Scenario: Typing in the search field

- **WHEN** the player types while the overlay is open
- **THEN** the characters reach the overlay's search field and the game receives no key events, so no ability is triggered and no game shortcut fires

#### Scenario: Clicking in the overlay

- **WHEN** the player clicks inside the overlay
- **THEN** the overlay receives the click and the player's character does not move or attack

#### Scenario: Overlay closes with a button still held

- **WHEN** the overlay is closed while a mouse button is held down
- **THEN** the game does not observe a press it never saw the start of

#### Scenario: Overlay is closed

- **WHEN** input arrives while the overlay is closed
- **THEN** it reaches the game unmodified

### Requirement: Open and close on an explicit player action

The overlay SHALL be opened and closed by a deliberate player action, SHALL start closed, and SHALL NOT open itself in response to game events.

The overlay SHALL close itself whenever the conditions that make it safe stop holding — in particular when no live world exists — rather than remaining open over a game that is loading or tearing down.

#### Scenario: Player opens the overlay

- **WHEN** the player performs the open action during play
- **THEN** the overlay opens, takes input, and stays open until the player closes it

#### Scenario: World goes away beneath an open overlay

- **WHEN** the world is torn down while the overlay is open
- **THEN** the overlay closes and returns input to the game

### Requirement: Available only while the client is running

The overlay SHALL be offered only while the Item Assistant client is running, and SHALL close and stop offering itself when the client stops.

The client is the only writer of the item database and the only consumer of completed transfers. Without it, a transfer made in-game would sit unrecorded and the same item could later be handed out a second time.

#### Scenario: Client is not running

- **WHEN** the player performs the open action while the client is not running
- **THEN** the overlay does not open

#### Scenario: Client stops while the overlay is open

- **WHEN** the client stops while the overlay is open
- **THEN** the overlay closes and input returns to the game
