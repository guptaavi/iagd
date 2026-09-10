## MODIFIED Requirements

### Requirement: Attach only into a running, playable game

The DLL SHALL refuse to install any hook unless the game has reached a state where patching it is safe. Refusal is the normal case, not an error: the injector attaches repeatedly while the player is at the loading screen or the character-select menu, and installing hooks there crashes the game.

Before installing anything, the DLL SHALL verify in order that: the game-state entry points it needs are resolvable, a game engine instance exists, the game reports it is not loading, the game reports it is not waiting (checked for both values of the flag the game takes), and the game engine reports itself online. Failing any of these SHALL abort the attach.

Once those conditions pass, the DLL SHALL distinguish between hooks it requires and hooks that depend on optional parts of the game. A hook whose targets are unavailable because the game loaded a different renderer or input implementation SHALL be skipped, with the reason logged, and the attach SHALL still be reported as successful. Only a hook the DLL cannot function without SHALL abort the attach.

The features carried by a skipped hook SHALL disable themselves rather than fail at the point of use, so that skipping a hook removes a feature and never leaves a partly installed one.

#### Scenario: Game is still loading

- **WHEN** the DLL is attached while the game reports that it is loading
- **THEN** no hook is installed, an aborted-injection report is sent to the client, the reason is written to the hook log, and the DLL reports failure to the loader so it is unloaded again

#### Scenario: Game entry points are unavailable

- **WHEN** the DLL is attached before the game's own modules have been loaded, so the game-state entry points cannot be resolved
- **THEN** the DLL SHALL treat this as "not ready yet" rather than an error, abort the attach, and MUST NOT call through any unresolved entry point

#### Scenario: Game is ready

- **WHEN** every readiness condition passes
- **THEN** the crash reporter is installed, every hook whose targets are available is enabled, the worker thread is started, and the DLL reports success to the loader

#### Scenario: Optional hook's targets are unavailable

- **WHEN** the game has loaded a renderer whose entry points the overlay's hooks cannot resolve
- **THEN** those hooks are skipped, the reason is logged, the overlay disables itself, item capture and deposit are installed as usual, and the attach is reported as successful

#### Scenario: Required hook's targets are unavailable

- **WHEN** a hook the DLL cannot function without cannot resolve its targets
- **THEN** the attach is aborted as it is today
