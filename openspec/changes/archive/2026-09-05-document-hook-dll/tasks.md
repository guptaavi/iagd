This change produces specifications only. There is no code to write — the "implementation" is verifying each documented requirement against `HookDll/` and against the C# code on the other side of each cross-process contract, correcting the spec where they disagree, and publishing the result as the project's baseline.

No file under `HookDll/`, `IAGrim/`, or `DllInjector/` is modified by any task below.

## 1. Structural validation

- [x] 1.1 Run `npx openspec validate document-hook-dll --strict` and verify it reports no errors — every requirement has at least one scenario, every scenario uses four hashtags, and every new capability has a Purpose of sufficient length.
- [x] 1.2 Verify the seven spec directories under `openspec/changes/document-hook-dll/specs/hook-dll/` match the seven capabilities listed in `proposal.md` exactly, with no extra or missing path.

## 2. Verify each capability against the source

Each task below is a read-only pass over the named files. Verification is the same for all of them: every requirement in that spec is traceable to code in those files, and any behaviour in those files that a downstream system relies on is covered by a requirement. Where the spec and the source disagree, correct the spec and note the correction in the task.

- [x] 2.1 Verify `injection-lifecycle` against `HookDll/Hook/dllmain.cpp`, and confirm the abort/retry protocol matches what `DllInjector/InjectionVerifier.cs` and `DllInjector/InjectionHelper.cs` actually look for — specifically that the `.PID` and `.ABORTED` filenames and their consumption match on both sides.
- [x] 2.2 Verify `game-interop` against `GrimTypes.h`, `GrimTypes.cpp`, `Exports.h`, `GameContext.cpp`, and `VTableDispatch.h`, and confirm the replica structure's asserted size and field offsets still match the game build the project currently targets.
- [x] 2.3 Verify `client-messaging` against `DataQueue.h`, `dllmain.cpp`, and `BaseMethodHook.cpp`, and confirm every value in `MessageType.h` matches `IAGrim/UI/Misc/MessageType.cs` by both name and number, recording each value that exists on only one side.
- [x] 2.4 Verify `item-capture` against `InventorySack_AddItem.cpp`, and confirm the incoming file's format and folder match what the client's ingoing-queue reader expects.
- [x] 2.5 Verify `item-deposit` against `InventorySack_AddItem.cpp` and the deserialisation in `GrimTypes.cpp`, and confirm the outgoing, deleted, and mod/hardcore folder shapes match `IAGrim/Utilities/GlobalPaths.cs`.
- [x] 2.6 Verify `seed-info` against `OnDemandSeedInfo.cpp`, and confirm the request and result folder shapes and the result JSON's keys match what the client writes and reads.
- [x] 2.7 Verify `diagnostics` against `HookLog.cpp` and `CrashReporter.cpp`, and confirm the crash artifact names and location match what `IAGrim/Services/DiagnosticsReport.cs` collects.

## 3. Confirm the recorded drift

Each task confirms a claim made in `design.md` — Risks / Trade-offs. If a claim turns out to be wrong, correct `design.md` rather than leaving it.

- [x] 3.1 Confirm every source file listed in `Hook/CMakeLists.txt` that no longer exists is genuinely absent, and confirm `Custom.vcxproj` is the real build by checking which project `GDIAHook.sln` references.
- [x] 3.2 Confirm `INSTALOOT_ENABLE` is tested by no surviving source file, and confirm which configurations `copy.cmd` and `debug.cmd` actually ship.
- [x] 3.3 Confirm each message type listed as dead has no remaining sender in `HookDll/` and no remaining handler in `IAGrim/`, so the list in `design.md` is accurate before anyone acts on it.

## 4. Publish the baseline

- [ ] 4.1 Have the specs reviewed by someone with working knowledge of the DLL, and fold their corrections in — particularly on the Open Questions in `design.md`, where source comments are the only evidence available.

  > Left open deliberately at archive time. The baseline was published un-reviewed by choice; the Open Questions in `design.md` remain open and are the place to start. Everything verifiable from source was verified in groups 1-3, including the replica layout, which was confirmed against the decompiled `ItemReplicaInfo::operator=`.
- [x] 4.2 Sync the delta specs into `openspec/specs/hook-dll/**` and verify the seven main specs exist there with their Purpose sections intact and no `TBD` placeholder remaining.
- [x] 4.3 Archive the change and verify `npx openspec list` shows the seven `hook-dll/*` capabilities as the project's baseline.
