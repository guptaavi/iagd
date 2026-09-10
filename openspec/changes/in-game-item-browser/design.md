## Context

See `proposal.md` for motivation. The constraints that actually shape the approach:

**The game exports what an overlay normally has to steal.** `Direct3D11.dll` exports `?GetDevice@Direct3DDevice11@GAME@@QEBAPEAUID3D11Device@@XZ` alongside `BeginFrame`, `EndFrame` and `PresentSurface`. `DirectInput.dll` exports `GAME::DirectInputDevice`'s `GetNumMouseEvents` / `GetMouseEvent` / `GetNumKeyEvents` / `GetKeyEvent` / `IsButtonDown` / `GetCursorPosition` / `Update`. Both are reachable by decorated name through the existing `GetProcAddressOrLogToFile` + Detours pattern used throughout `HookDll/Hook`.

**Two renderers ship, one is in scope.** The install carries `Direct3D.dll` (D3D9) and `Direct3D11.dll`; `Grim Dawn.exe` also accepts `/d3d12` but no `Direct3D12.dll` ships, so that switch is dead.

Measured rather than assumed, by inspecting the loaded modules of a running process: launching with **no argument loads `direct3d11.dll`**, so Direct3D 11 is what players get by default and the overlay is available to them. The renderer is selected with `/renderer direct3d11` or `/renderer direct3d9`; the bare `/d3d11` form did not select Direct3D 11 in testing, so use the `/renderer` form when testing the fallback path.

**The data is already prepared.** `%LOCALAPPDATA%\EvilSoft\IAGD\` holds `data\userdata.db` (WAL, ~286 MB on a played account) and `storage\*.tex.png` (~4,500 extracted icons). `SettingsReader.cpp` already resolves this folder. `ReplicaItemRow` stores the game's own tooltip lines as `{type, text}` with `^K`/`^S`/`^M` colour markers.

**The search is already raw SQL.** `IAGrim/Database/DAO/PlayerItemDaoImpl.cs:807` builds a string and binds parameters; there is no ORM behaviour to reproduce.

**The build is the weak link.** `HookDll/Hook/Custom.vcxproj` was a v140 project with absolute `IncludePath`/`LibraryPath` entries pointing at specific boost and MSVC installs on specific machines. It consumed exactly one prebuilt library (Detours). Only `Release|x64` was buildable at all; the Win32, `Release-Instaloot` and `Release-playtest` configurations reference a boost that no longer exists and an XP SDK that is not installed.

## Goals / Non-Goals

**Goals:**

- A seam between the UI toolkit and everything else, so the toolkit is replaceable without touching search, presentation or transfer.
- Every game call on the thread the game expects, and nothing expensive inside a frame.
- One writer to `userdata.db`, and it is not the DLL.
- Failures degrade to "the overlay is unavailable", never to a crashed game.

**Non-Goals:**

- Sharing code with the .NET client. The port is deliberate duplication; see the risk below.
- A general-purpose overlay framework. This draws one application.
- Rendering anything the client renders that the specs do not name.

## Decisions

### RmlUi over Dear ImGui

**Chosen: RmlUi, using its upstream `RmlUi_Renderer_DX11` backend unmodified.**

The deliverable is a content browser — an icon grid with quality frames, colour-coded rich text, and a dense filter sidebar. That is the genre immediate-mode UI is worst at. RmlUi's RCSS also maps almost one-to-one onto the styling the WebUI already expresses (`replica-letter-K`, `replica-type-N`, the rarity backgrounds), so the visual work is largely transcription rather than invention. Runtime RML/RCSS reload makes iterating on look far cheaper than recompiling and reinjecting a DLL.

*Dear ImGui* was the alternative and was genuinely competitive on integration cost — six files, no build system change, ~0.4 MB — against RmlUi's CMake build plus FreeType and ~2-4 MB. The decisive factor was that ImGui's main structural advantage here evaporated once D3D9 left scope: `imgui_impl_dx9.cpp` exists and RmlUi has no D3D9 renderer, but with D3D9 out, both toolkits ship the backend we need.

An intermediate plan — stand up ImGui first to de-risk the device and input hooks, then replace it with RmlUi — was considered and dropped. The DX11 backend is upstream code in both cases, so the throwaway phase would have de-risked only the hooks themselves, which a solid-colour quad proves just as well without a second toolkit in the tree.

### The x64 hook moves from the v140 toolset to v143

RmlUi's *public headers* require C++17 — `Include/RmlUi/Core/Traits.h` uses `if constexpr` and
`std::is_same_v`, and `Config/Config.h` uses `std::is_same_v` throughout. Both are included by
essentially everything, so this is not avoidable by building RmlUi separately: any translation unit
that talks to RmlUi must itself compile as C++17. v140 (VS2015) predates `if constexpr` entirely.

The x64 configurations therefore build with **v143 and `/std:c++17`**. Win32 and the two dead
configurations are left on v140; they do not build for unrelated reasons and nothing ships from them.

**This was verified against the running game rather than argued from the documentation**, because the
hook passes MSVC STL objects directly into Grim Dawn's own code and a mismatch there corrupts memory
rather than failing to link. Grim Dawn imports `MSVCP140.dll` / `VCRUNTIME140.dll`, which is the
*shared* runtime family for v140 through v143, not a VS2015 marker; Microsoft's binary-compatibility
guarantee covers the whole 14.x range. Injecting a v143-built hook into a live game confirmed it:

- all 16 game exports resolved, all hooks installed, `VTableDispatch` slot 139 resolved
- `ShowCinematicText` displayed a message from hook-allocated `std::wstring`s
- **the game `push_back`ed 44 and then 59 `GameTextLine` structs into a hook-allocated
  `std::vector`** via `VTableDispatch::Call`, and both items persisted correctly
- no errors, no fatals, no crash; the game stayed responsive

That third point is the real test: allocation on one side of the toolset boundary and mutation on the
other, with live data.

Two consequences to keep in view:

- The hook must stay `/MD` (it is). The `std::vector` above is allocated by the hook and filled by the
  game, so both sides must share one CRT heap. A static CRT would corrupt that, which rules out
  "link the runtime statically to avoid the redistributable dependency".
- The v143 build additionally imports `VCRUNTIME140_1.dll`, so the redistributable floor rises. This
  was accepted deliberately: Linux/Wine users can install a current runtime.

*Alternatives considered:* keeping v140 and using Dear ImGui (C++11, no toolset change) was rejected
for the reasons in the toolkit decision above. Splitting the overlay into a separate v143 DLL behind
an `extern "C"` boundary, leaving the hook on v140, was considered and rejected once the runtime
floor was accepted as acceptable — it bought crash isolation at the cost of a second shipped module
and an ABI to maintain, and its main advantages had evaporated.

### Hook `PresentSurface`, not `EndFrame` and not `IDXGISwapChain::Present`

`?PresentSurface@Direct3DDevice11@GAME@@UEAAXPEAVRenderSurface@2@@Z` is handed the `Direct3DDevice11` instance as `This`, and `GetDevice(This)` returns the `ID3D11Device*`. That removes the entire standard overlay ritual: no dummy device, no vtable walk, no signature scan.

**`EndFrame` was tried first and is wrong**, which is worth recording because it looks right. Drawing there put a rectangle on screen *and* tinted the whole frame red with blurry scaled copies of it. Logging the bound render target explained why: it reports `BindFlags = RENDER_TARGET | SHADER_RESOURCE`. A real back buffer is not a shader resource — the game reads that texture back in shaders, so `EndFrame` is the end of the 3D pass and anything drawn into it is fed through the bloom and composite passes. `PresentSurface` runs after all of that, and drawing there leaves the rest of the frame pixel-identical.

The render target is recovered with `OMGetRenderTargets` at hook time rather than by tracking the swap chain, since `CreateSwapChain` is private and returns nothing.

State save/restore is **not** needed by the current bring-up draw, which uses `ID3D11DeviceContext1::ClearView` — it binds no shaders, no input layout and no vertex buffers, so it cannot disturb the next frame. It becomes necessary the moment RmlUi's pipeline replaces it; the upstream DX11 backend already does most of that work.

Symbols are resolved at runtime by decorated name via `GetProcAddressOrLogToFile`, *not* by linking the `compat/Direct3D11.lib` import library that ships with the game. Runtime resolution degrades to a logged warning when a patch moves things, which is how the rest of the hook survives game updates, and it keeps the build independent of the user's game install.

### Suppress the game's input at its own event queue; feed the overlay from window messages

These are two jobs, and they are best served by two different mechanisms.

**Suppression** hooks the game's own input layer: while the overlay is open, `GetNumMouseEvents` and `GetNumKeyEvents` return zero and `IsButtonDown` returns false. The game sees an empty queue rather than events it must be persuaded to ignore.

Only those three are hooked. `GetMouseEvent`, `GetKeyEvent` and `GetCursorPosition` return class types *by value* — `ButtonEvent` and `MouseEvent` have virtual destructors, so they come back through a hidden return pointer and their layouts are unpublished. Reading them would put a reverse-engineered struct on the critical path, where a game patch could change it silently. Suppression does not need them: an empty queue is never indexed, so those functions are not called at all.

**Overlay input** therefore comes from a WndProc subclass instead, which is what RmlUi's `RmlUi_Platform_Win32` backend already consumes. WndProc alone could never have *stopped* the game — that was the original objection to it — but paired with the DirectInput suppression above it does not need to.

*Alternative considered:* hooking `IDirectInputDevice8::GetDeviceState` would also work, but needs vtable hooking of a COM object the game creates, against exported virtuals that are already right there.

**Held-button state is the subtle part.** Returning false while suppressing and the real state afterwards means a button still held when the overlay closes is handed to the game as a press whose start it never saw. Buttons held across a suppression transition have to stay suppressed until they are released.

### The DLL is a read-only database consumer; the client owns all writes

The DLL opens `userdata.db` with `SQLITE_OPEN_READONLY`. Transfers are handed back through an append-only journal under the client's storage folder, published atomically (write to `.tmp`, then `MoveFile`) exactly as `InventorySack_AddItem::Persist` already does.

*Alternative considered and rejected:* having the DLL delete rows directly. It would have to reproduce the `DeletedPlayerItem` bookkeeping, the `cloudid` marking, the `ComputedItemStat` / `ReplicaItem2` cascade deletes (`PlayerItemDaoImpl.cs:229-270`) and the stack-merge semantics — in C++, against a schema NHibernate migrates. That duplication would break on the first schema change.

**Ordering: place the item, then journal.** A journal entry written before placement that then fails costs the player an item. An item placed but not journalled leaves a duplicate the player can delete. The window is one `MoveFile` wide and the failure mode is recoverable, so the ordering favours never destroying an item. Each entry carries a GUID; the client tracks applied ids so a crash mid-consume cannot decrement a stack twice.

### Change detection via `PRAGMA data_version`

WAL is already enabled (`EnableWalJournalMode.cs`), so a second process can read while the client writes. `PRAGMA data_version` changes when another connection commits, which gives a cheap poll for "the client changed something, refresh". Polled from the overlay's worker, not from a frame.

*Alternative:* file mtime — unreliable under WAL, since commits land in the `-wal` file.

### Search runs on a worker; frames only consume results

A search is posted to a worker thread and the completed result handed back through the existing `BaseDataQueue`. Superseded searches carry a generation counter and are dropped on completion. Nothing in the frame path touches SQLite.

### Layering

```
+------------------------------------------------------------+
|  Shell        RmlUi documents, RCSS, event listeners        |
|               (the only code that knows the toolkit)        |
+------------------------------------------------------------+
                 | SearchQuery / ItemView[] / TransferRequest
                 v
+------------------------------------------------------------+
|  Core         query builder -> sqlite3 (read-only)          |
|               replica row parser (^K markers, row types)     |
|               icon resolution + texture cache                |
|               stack merge                                    |
|               transfer journal writer                        |
|               -- no game types, no toolkit types --          |
+------------------------------------------------------------+
                 | ID3D11Device* / input events / game thread
                 v
+------------------------------------------------------------+
|  Host         Direct3DDevice11::EndFrame hook               |
|               DirectInputDevice::* hooks + suppression       |
|               GameEngine::Update -> item creation            |
+------------------------------------------------------------+
```

Core is the layer worth testing, and it depends on neither the game nor RmlUi. It should be buildable and runnable against a copy of `userdata.db` outside the game, which is how the SQL port gets verified against the C# original without a game running.

### Dependencies

| Library | Why | Notes |
|---|---|---|
| RmlUi | UI | Static lib, CMake. `RmlUi_Renderer_DX11` used unmodified. |
| FreeType | RmlUi's font engine | Required by RmlUi's default configuration. |
| sqlite3 amalgamation | database | Single `.c` file, no build system. |
| PNG decoder | item icons | `stb_image` unless RmlUi's sample decoder suffices. |

`HookDll/Shared/SQLite.cpp` and its `IDB` / `ITable` interfaces are not reused. They are in no solution, vendor no `sqlite3.h`, and expose a string-matrix API unsuited to prepared statements. They predate this project and should be deleted rather than revived.

## Risks / Trade-offs

**The build integration is the real schedule risk, not the graphics.** → `Custom.vcxproj` must consume a CMake-built static library before any of this can compile. Treat de-hardcoding the boost/MSVC paths and adding a CMake dependency as the first task, done and verified on its own, not folded into feature work.

**A path that looked dead was holding the v140 build together.** → The old `IncludePath` put an SDK 10.0.10240 `ucrt` directory first, which pinned the v140 compiler to UCRT headers old enough not to use `_mm_loadu_si64`. Removing it as "obviously stale" broke the build immediately. It is now `UcrtCompatIncludeDir` in `HookDll/Directory.Build.props`, empty by default because v143 does not need it, and documented so the next person does not repeat the removal.

**C++17 deprecates `<codecvt>`, which the hook uses in five files.** → Silenced with `_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING` rather than refactored mid-migration; `std::wstring_convert` still works. Replacing it with `MultiByteToWideChar`/`WideCharToMultiByte` is worth its own change, and should not be entangled with this one.

**Every bug now crashes the game, not the client.** → All new hooks follow the existing discipline: try/catch with a `catch(...)` fallback around every hooked body, `LogToFile` with flush above INFO, and the original always called. Note the static-initialisation-order warning at `dllmain.cpp:22` — it applies to anything new with namespace-scope state.

**The search query now exists twice, in C# and C++, and will drift.** → Port it as a literal, annotated transcription with a comment at the top of each file naming the other, and keep the C++ side a deliberate subset so there is less surface to diverge. The subtleties that must survive the port: the sargable `retaliation >= prefix AND < prefix++` range (a `LIKE` cannot use the index), the `IFNULL` wrapping in `PetRecordCondition` (a `NOT IN` with any NULL operand matches nothing), and the deterministic `ORDER BY name, Id` that makes paging stable.

**Two UIs over one dataset go stale in both directions.** → `PRAGMA data_version` covers the overlay's direction. The client's direction is out of scope here; a player who transfers in-game sees the client's grid refresh on its next search.

**Duplicate item if the game dies between placement and journal.** → Accepted, and deliberately preferred over the reverse failure. The window is one atomic file publish.

**RmlUi allocating inside the game process.** → Give RmlUi's `SystemInterface` the existing logging path so its diagnostics land in the hook log, and cap the result page so document size stays bounded.

**Wine / Linux.** → The DLL already has a Wine path (`g_isRunningInWine`, the linuxhack folder). An overlay under Wine plus DXVK is a separate validation matrix and is not assumed to work; if it does not, the overlay disables itself under Wine and the client path is unaffected.

## Open Questions

- Which font ships with the DLL, and under what licence. English-only for this change keeps the glyph set small, but the choice should not foreclose adding ranges later.
- Whether the open/close action should be a fixed key or configurable through the client's `settings.json`. The DLL already reads that file, so making it configurable is cheap, but a fixed default has to be chosen either way and must not collide with a game binding.
- Whether the overlay should offer any filter the client does not have. Nothing requires it; worth revisiting once the sidebar subset is in front of a player.
