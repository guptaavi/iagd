## Context

See `proposal.md` — Why. This design section doubles as the architectural map of `HookDll/` that the proposal says is missing, so that a change scoped to the C++ DLL can be planned against something concrete.

### Layout

```
HookDll/
  Hook/                 the DLL itself, builds ItemAssistantHook_x64.dll
  Shared/               older shared utilities (SQLite, unicode, file dialogs)
  Detours-master/       vendored Microsoft Detours
  copy.cmd, debug.cmd   post-build copy into the IAGrim output folders
```

`Hook/` is roughly 3,700 lines across ~20 translation units; `Shared/` is another ~700 and is largely legacy — the DLL uses only its precompiled-header plumbing, not its SQLite or dialog code.

### Threads

Five threads matter, and almost every hazard in this codebase is one of them touching something owned by another:

| Thread | Owns | Notes |
| --- | --- | --- |
| Game update thread | `GameEngine::Update` hook | deposits; resolves the world context |
| Game render thread | `Engine::Render` hook | seed-info stat generation |
| Whatever thread adds items | `InventorySack::AddItem` hooks | item capture |
| Worker thread | message delivery to the client | started by the DLL |
| Two polling threads | deposit discovery, seed-request discovery | file I/O only, never call into the game |

The rule the code follows: **file I/O happens on polling threads, game calls happen on game threads, and the two communicate through a locked queue or set.** The world context (mod name + hardcore) is the one piece of game state a polling thread needs, so it is read on the game thread and cached behind a mutex.

### Data flow

```
capture:  game thread ──► itemqueue/ingoing/*.csv                              ──► client
deposit:  client ──► itemqueue/outgoing/{sc,hc}[/mod]/*.csv ──► poll thread ──► game thread
                                                            └──► itemqueue/deleted/... (soft delete)
stats:    client ──► replica/from_ia[/mod]/*.csv ──► poll thread ──► render thread ──► replica/to_ia/*.json
messages: any thread ──► DataQueue ──► worker thread ──► WM_COPYDATA  (native)
                                                     └──► linuxhack/*.msg (Wine)
```

Every one of those file handoffs uses the same publish trick: write under a name the reader ignores, then rename. It is the only cross-process atomicity available here.

### Class map

- `dllmain.cpp` — entry point, readiness gate, worker thread, Wine bridge, logging shims.
- `BaseMethodHook` — base class; wraps Detours attach/detach and reports hook success/failure to the client.
- `InventorySack_AddItem` — despite the name, this is both capture *and* deposit; it owns `GameEngine::Update`, both `AddItem` overloads, `SetTransferOpen`, and the deposit polling thread. The largest and most tangled unit at ~930 lines.
- `OnDemandSeedInfo` — stat generation; owns `Engine::Render` and `SetDifficultyRamp` plus the seed-request polling thread.
- `GetPrivateStash`, `SetHardcore` — small single-purpose hooks.
- `GrimTypes` — the mirrored `GAME::` structures, all export resolution, world-alive check, replica serialisation.
- `GameContext` — the cached per-world mod name and hardcore flag.
- `VTableDispatch` — virtual-slot resolution for `GetUIDisplayText`.
- `HookLog`, `CrashReporter` — diagnostics.
- `DataQueue` — the thread-safe queue template, used for both client messages and parsed seed requests.

## Goals / Non-Goals

**Goals:**

- Produce a baseline the next change can amend rather than re-derive.
- Write down the contracts that cross a process boundary — file formats, folder layout, message numbering, injection protocol — since those are the ones that break silently.
- Write down the constraints that exist because this code runs inside someone else's process, so a future change does not violate one by accident.
- Record the drift and unknowns found while reading, rather than leaving them to be rediscovered.

**Non-Goals:**

- Any behaviour change, refactor, or new hook.
- Specifying the C# side. It is described where it is the counterparty to a contract; its own behaviour is out of scope.
- Documenting `Shared/` beyond noting that it is mostly unused by this DLL.
- Documenting vendored Detours.

## Decisions

### Seven capabilities rather than one

The DLL is one binary, but its failure modes do not correlate. A change to stat generation cannot break injection; a change to injection can break everything. Splitting along those lines means a future change amends one small spec instead of one large one, and the blast radius is legible from the file it touches.

*Alternative considered:* a single `hook-dll` spec. Rejected — it would be long enough that nobody re-reads it, which is the failure mode this change exists to fix.

*Alternative considered:* splitting by source file. Rejected — `InventorySack_AddItem` alone would own both capture and deposit, which are independent behaviours with independent folders, threads, and failure modes. The split follows behaviour, not files.

### Capture and deposit are specified separately despite sharing a class

They share only the settings reader and the notification helper. They have different directions, different folders, different trigger points, and different failure modes. Specifying them together would make either one harder to change. That the code puts them in one class is an observation for the risk table below, not a reason to mirror it in the specs.

### Specs describe behaviour; mangled symbol names stay out of them

Specs name *what* is resolved ("the display-text method for the item's class"), not the mangled symbol string. The strings live in `Exports.h` and in the source, change with every game patch, and would make the specs stale on a schedule nobody controls. `game-interop` specifies the *rule* — resolve by exported symbol, degrade when missing — which is the part that must hold across patches.

### Documented from source, not from a running game

Everything here was derived by reading `HookDll/` and the C# code on the other side of each contract. It has not been validated against a live game or a debugger. Statements the source itself marks as uncertain are carried through as uncertain rather than resolved by guessing — see Open Questions.

## Risks / Trade-offs

**The specs describe current behaviour, including behaviour that may be wrong** → Requirements are written from what the code does. Where the code's own comments flag something as a known problem, the spec records the constraint rather than blessing the workaround. A future change is free to propose different behaviour; the baseline just makes the change visible.

**`InventorySack_AddItem` owns two unrelated capabilities plus the update hook** → At ~930 lines it is the most likely place for a future change to cause collateral damage, and its shared static state makes it hard to reason about. Recorded here so a change that touches it is planned with that in mind; splitting it is a separate proposal.

**`Hook/CMakeLists.txt` is stale** → It lists a dozen source files that no longer exist (`CanUseDismantle`, `CloudRead`/`CloudWrite`/`CloudGetNumFiles`, `NpcDetectionHook`, `SaveTransferStash`, `SetTransferOpen`, `StateRequestMoveAction`, `StateRequestNpcAction`) and declares an executable rather than a library. The real build is `Custom.vcxproj` / `GDIAHook.sln`. Anyone who tries to build via CMake will conclude the tree is broken. The CMake file is IDE indexing support, not a build.

**Dead message types remain in the shared numbering** → `TYPE_CloudGetNumFiles`, `TYPE_CloudRead`, `TYPE_CloudWrite`, `TYPE_GameInfo_SetModName`, `TYPE_Stash_Item_BasicInfo`, and `TYPE_ITEMSEEDDATA_PLAYERID` have no remaining sender in the DLL. Their numbers must stay reserved. Removing them is a separate change that must touch both sides.

Two of them are dead senders with live client code still waiting on them, so "unused" is not quite true from the client's point of view: `MainWindow.cs` keeps a `case` for `TYPE_GameInfo_SetModName` that logs a message the DLL can no longer send, and `GenericErrorHandler.cs` special-cases hook identifiers 11, 12 and 13 — the Steam cloud hooks — to advise GOG users that a failure there is normal, for hooks the DLL no longer installs. Both are orphaned handlers, harmless but misleading to anyone reading the client to work out what the DLL sends.

**The two enumerations have drifted in both directions** → 15 values match by name and number. `TYPE_GAMEENGINE_SetDifficultyRamp = 8001` exists only in the DLL, and `TYPE_CINEMATIC_TEXT = 9000` only in the client. The 8001 gap has a visible consequence: hook identifiers travel as the *payload* of `TYPE_SUCCESS_HOOKING_GENERIC` / `TYPE_ERROR_HOOKING_GENERIC`, and `GenericErrorHandler.cs` casts that payload back to its own enumeration to name it — so a successful `SetDifficultyRamp` hook logs as the bare number rather than a name. The same is true of the literal `2` that `SetTransferOpen` uses as its hook identifier, which appears in neither enumeration. Hook identifiers are therefore part of the shared numbering in practice, whether or not they are ever used as a message type.

**Four build configurations, one of which nothing consumes** → `Debug`, `Release`, `Release-playtest`, and `Release-Instaloot` exist for both x64 and Win32. `copy.cmd` ships only `Release` and `Release-playtest`, both x64. `Release-Instaloot` defines `INSTALOOT_ENABLE`, which no surviving source file tests. Win32 is not shipped. Retail and playtest differ in the game binary they target, which is why two ship.

**Pinned to the v140 / v140_xp toolsets** → Modern language features are unavailable, and this constrains any new dependency. Changing it is a build-infrastructure change with its own risk profile.

**Boost and Detours are load-bearing** → Boost `property_tree` parses settings and emits result JSON, Boost `filesystem` walks the queue folders, Boost `thread` provides the deposit mutex. Detours performs every function patch. Neither can be swapped casually inside an injected DLL.

**The mirrored item-replica layout is version-locked** → It is asserted to be exactly 0x190 bytes for retail FOA v1.3, with two trailing fields present solely to make the size right. A game update that grows the structure produces stack corruption at the capture site, not a clean failure. The static assertion catches only a mismatch that someone already noticed; it cannot detect that the *game* changed.

**Nothing on the client side surfaces the crash artifacts** → The crash reporter exists so that a user who cannot run a debugger has only to send some files. It writes `crash_<timestamp>_<pid>_<index>.dmp` / `.txt` / `.log` into the IAGD data folder. No C# code references `crash_` or `iagd_hook.log` anywhere: `DiagnosticsReport.cs` reports the bridge folder, the injection markers, and the queue counts, but never the crash reports or the hook log. So the evidence is produced and then not asked for. Closing that gap is a client-side change, but it is recorded here because it determines whether the diagnostics capability actually pays off.

**The two queue folder shapes are not symmetric** → The deposit queue is split by hardcore *and* mod (`itemqueue/outgoing/{sc,hc}[/mod]`); the seed-request queue is split by mod only (`replica/from_ia[/mod]`). The DLL's request-folder helper still takes a hardcore argument and ignores it, which reads like an oversight but matches what the client writes. A change that "fixes" the signature must not change the path.

**Mixed synchronisation primitives** → `std::mutex` in most places, `boost::mutex` for the deposit set, `boost::shared_ptr`/`shared_array` in the queue alongside `std::atomic`. Not a defect, but a future change should follow whatever the file it edits already uses rather than introducing a third convention.

## Migration Plan

Not applicable — specs only. Archiving this change creates `openspec/specs/hook-dll/**`; no artifact ships and there is nothing to roll back beyond deleting the specs.

## Open Questions

These are recorded because the source itself marks them unknown. None of them change a requirement above, and each can be answered later by a change that needs the answer.

- The boolean argument to the game's `IsGameWaiting` is undocumented; the attach gate calls it with both values and treats either "waiting" result as a reason to abort. Two `TODO`s in the source ask what it means.
- Two trailing fields in the mirrored replica structure exist only to make the size match; their purpose is unknown.
- `fnIsWorldAlive` deliberately avoids `IsGameWaiting(false)` even though it looks like the stronger check, because it also demands a player state value and a player flag whose meanings are unknown, and either could silently disable capture.
- The stat-generation path notes it receives more replies than expected, attributed to multiple classes in the hierarchy carrying hooks. Never root-caused.
- In-game notification text is English-only; the source carries a `TODO` about translation support.
- `GameEngineUpdate` is a complete, working hook class that nothing constructs — its registration in `dllmain.cpp` is commented out and marked debug/test only. Whether to keep it is a separate decision.
