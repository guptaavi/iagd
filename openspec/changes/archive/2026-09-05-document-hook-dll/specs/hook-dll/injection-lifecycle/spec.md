## Purpose

Governs how the hook DLL enters and leaves the Grim Dawn process: the readiness conditions the game must satisfy before any function is patched, the guarantee that only one copy of the hook is ever active, and the protocol by which a refused attach is reported back to the injector so it can retry rather than declare failure.

## ADDED Requirements

### Requirement: Attach only into a running, playable game

The DLL SHALL refuse to install any hook unless the game has reached a state where patching it is safe. Refusal is the normal case, not an error: the injector attaches repeatedly while the player is at the loading screen or the character-select menu, and installing hooks there crashes the game.

Before installing anything, the DLL SHALL verify in order that: the game-state entry points it needs are resolvable, a game engine instance exists, the game reports it is not loading, the game reports it is not waiting (checked for both values of the flag the game takes), and the game engine reports itself online. Failing any of these SHALL abort the attach.

#### Scenario: Game is still loading

- **WHEN** the DLL is attached while the game reports that it is loading
- **THEN** no hook is installed, an aborted-injection report is sent to the client, the reason is written to the hook log, and the DLL reports failure to the loader so it is unloaded again

#### Scenario: Game entry points are unavailable

- **WHEN** the DLL is attached before the game's own modules have been loaded, so the game-state entry points cannot be resolved
- **THEN** the DLL SHALL treat this as "not ready yet" rather than an error, abort the attach, and MUST NOT call through any unresolved entry point

#### Scenario: Game is ready

- **WHEN** every readiness condition passes
- **THEN** the crash reporter is installed, every hook is enabled, the worker thread is started, and the DLL reports success to the loader

### Requirement: Only one copy of the hook per process

The DLL SHALL refuse to initialise if a copy of the hook is already active in the target process. Two copies would patch the same game functions over each other's trampolines and run duplicate background threads against the same files, which reliably crashes the game. Loading the same DLL under a second filename is not prevented by the operating system, so the DLL SHALL enforce this itself, scoped to the current process so that two separate game processes are unaffected.

If the DLL cannot determine whether another copy is present, it SHALL proceed rather than block a legitimate attach.

#### Scenario: Second copy attaches

- **WHEN** the DLL is attached to a process where the hook is already active
- **THEN** initialisation is refused before any other state is touched, the refusal is logged, and the already-attached copy's resources — in particular any injection marker files it owns — are left intact

### Requirement: Report an aborted attach so the injector can distinguish it from a failure

When an attach is refused, the DLL SHALL notify the client that injection was cancelled. Under Wine the DLL SHALL additionally write a dedicated marker file, named for the current process id, that the injector can check synchronously immediately after injecting.

The marker is required because the ordinary notification cannot carry this news in time: a refused attach unloads the DLL, which removes the process-id file the injector verifies against, while the message itself is drained asynchronously by the client's poll timer.

#### Scenario: Attach refused under Wine

- **WHEN** an attach is refused and the DLL is configured for Wine
- **THEN** an abort marker file for the current process id is written to the bridge folder and an injection-cancelled message is queued, and the marker is re-written on every subsequent aborted attach

#### Scenario: Attach refused natively

- **WHEN** an attach is refused and the DLL is running natively
- **THEN** an injection-cancelled message is sent directly to the client window if one is present, and nothing is written if no client window can be found

### Requirement: Signal successful injection under Wine

When configured for Wine, the DLL SHALL create its bridge folder and write a file named for the current process id on attach, and SHALL delete that file on detach. This file is the only signal the injector uses to confirm that injection took effect.

#### Scenario: Successful attach under Wine

- **WHEN** the DLL attaches successfully in Wine mode
- **THEN** the bridge folder exists and contains a process-id file for this process

### Requirement: Detach without leaving the game patched

On detach the DLL SHALL leave the game in a state where nothing points into the unloading module. It SHALL, in order: remove the crash handler, stop background threads that call into the game, disable and destroy every installed hook, stop the worker thread, and release its process-wide resources.

Detaching a hook SHALL restore the original target through the same pointer the hook installer wrote, and SHALL be a no-op for a hook that never installed. A hook left installed after unload means the game jumps into freed memory.

#### Scenario: Detach after a partial hook installation

- **WHEN** the DLL detaches after some hooks failed to install because their targets could not be resolved
- **THEN** the successfully installed hooks are removed and the failed ones are skipped without error

#### Scenario: Detach ordering

- **WHEN** the DLL detaches while background threads are running
- **THEN** those threads are stopped before hooks are removed, so none of them can call into the game through a half-removed hook
