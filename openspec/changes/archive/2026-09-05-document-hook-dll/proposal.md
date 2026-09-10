## Why

`HookDll/` is the C++ DLL that Item Assistant injects into Grim Dawn. It is the riskiest code in the repo — it runs inside someone else's process, patches shipped game functions by mangled export name, and mirrors game struct layouts byte-for-byte — yet it has no written specification. Everything that is known about it lives in source comments, in the head of whoever last debugged a crash, and in Ghidra findings that were never written down.

New functionality is about to be built inside that DLL. Without a baseline of what the DLL is contractually supposed to do, every proposal for new work has to re-derive the existing behaviour from 5,400 lines of source, and there is nothing to check a change against when it breaks looting, deposits, or the Wine bridge.

## What Changes

- Establish a **baseline specification** for the existing HookDll behaviour, split into seven capabilities under `hook-dll/`. This is documentation of what the DLL already does, derived by reading the source — no behaviour changes and no code edits.
- Record the contracts that are currently implicit and easy to break:
  - the injection preconditions and the abort/retry protocol the C# injector depends on,
  - the on-disk queue formats (`itemqueue/`, `replica/`, `linuxhack/`) shared with the C# client,
  - the `WM_COPYDATA` message-type numbering that must stay in lockstep with `IAGrim/UI/Misc/MessageType.cs`,
  - the `GAME::ItemReplicaInfo` layout guarantee and the "resolve exports by mangled name, degrade instead of crash" rule that lets the DLL survive a game patch.
- Capture the known drift and hazards found while reading the code, so future work starts from an accurate picture rather than rediscovering them (see `design.md`): the stale `CMakeLists.txt`, the `Release-Instaloot` configuration nothing builds, the `GameEngineUpdate` debug-only hook, and the two `IsGameWaiting` boolean arguments whose meaning is still unknown.

**Non-goals:** no runtime behaviour changes, no refactoring, no new hooks. This change produces specs only.

## Capabilities

### New Capabilities

- `hook-dll/injection-lifecycle`: `DllMain` attach/detach, the game-readiness preconditions that gate injection, single-instance enforcement, and how an aborted attach is reported to the injector on both Windows and Wine.
- `hook-dll/game-interop`: the rules for reaching into the game — mangled-export resolution, the mirrored `GAME::` struct layouts, virtual-slot dispatch for `GetUIDisplayText`, the world-alive check, and the cached per-world game context (mod name + hardcore).
- `hook-dll/client-messaging`: the queue-plus-worker-thread transport that carries messages to the IA client, over `WM_COPYDATA` natively and over atomically-renamed `.msg` files under Wine, plus the shared `MessageType` numbering.
- `hook-dll/item-capture`: "instaloot" — intercepting items added to the configured stash tab, filtering out what IA does not handle, and persisting the replica plus rendered stat lines to `itemqueue/ingoing/`.
- `hook-dll/item-deposit`: the reverse direction — polling `itemqueue/outgoing/`, recreating items on the game thread while the transfer stash is open, and soft-deleting the consumed CSV files.
- `hook-dll/seed-info`: on-demand item stat generation — reading replica requests from `replica/from_ia/`, building throwaway items during `Engine::Render` to render their tooltips, and writing the results as JSON to `replica/to_ia/`.
- `hook-dll/diagnostics`: the hook log (rotation, dedup, crash-safe flushing) and the self-reporting crash handler that writes a report, minidump, and log snapshot when the game faults.

### Modified Capabilities

None — `openspec/specs/` is currently empty, so every capability above is new.

## Impact

- **Specs only.** Adds `openspec/specs/hook-dll/**` once this change is archived. No source file under `HookDll/` is touched.
- **Documents cross-project contracts** that reach outside `HookDll/`: `IAGrim/UI/Misc/MessageType.cs`, `IAGrim/Utilities/GlobalPaths.cs`, `DllInjector/InjectionVerifier.cs`, and `IAGrim/UI/MainWindow.cs` all sit on the other end of the contracts specified here. Those files are described, not changed.
- **External dependency surfaces that constrain future work** get recorded: Detours 4.x, Boost (property_tree, filesystem, thread), the `v140` / `v140_xp` toolsets pinned in `Custom.vcxproj`, and the shipped Grim Dawn `Game.dll` / `Engine.dll` exports themselves.
