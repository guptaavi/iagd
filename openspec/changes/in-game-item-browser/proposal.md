## Why

Finding an item in Item Assistant means leaving the game: alt-tab to the client, search, transfer, alt-tab back, open the transfer stash. The hook DLL already runs inside Grim Dawn, already reads the client's settings, and already creates items in the transfer stash — everything needed to do that search in-game is present except a way to draw and drive a UI.

Grim Dawn also turns out to be unusually cooperative here. Its DirectX 11 renderer exports an accessor for the underlying `ID3D11Device`, and its input layer exports the per-frame mouse and key event queues as hookable virtuals. Both of the hard problems in building a game overlay are solved by hooking exported symbols in the style the DLL already uses everywhere else.

This complements the .NET client rather than replacing it. The client stays the primary interface and the only writer of the database.

## What Changes

- The hook DLL gains an in-game overlay: a search field, a subset of the client's stat filter sidebar, a result grid of matching player items, and a stat pane for the selected item.
- The overlay renders through **RmlUi** on the game's DirectX 11 renderer, driven from a hook on `GAME::Direct3DDevice11::EndFrame`.
- Input is captured by hooking `GAME::DirectInputDevice`'s event-queue accessors, which also lets the overlay suppress input to the game while it is open — without touching the window procedure or DirectInput itself.
- The DLL gains a **read-only** SQLite connection to the client's `userdata.db`, and a port of the client's player-item search query.
- Transferring an item from the overlay creates it directly in the transfer stash in-process, then records the transfer in an on-disk journal that the client consumes to perform the deletion and cloud bookkeeping. The CSV round trip is not used for this path.
- **The DirectX 9 renderer is out of scope.** When the game is not running the DirectX 11 renderer, the overlay disables itself and logs the reason; every existing hook behaves as it does today.
- The existing client-driven deposit path (`itemqueue\outgoing` CSV polling) is left untouched and keeps working alongside the new path.

### Non-goals

- Replacing the .NET client, its sidebar, or the WebUI.
- Buddy items, the Help tab, the Collections tab, or item comparison.
- Parsing the game database. The overlay reads what the client has already parsed.
- Writing to `userdata.db` from the DLL. All mutation stays in the client.
- Localisation. The overlay is English-only in this change; the client remains fully localised.

## Capabilities

### New Capabilities

- `hook-dll/overlay-host`: Acquiring the game's render device and drawing a UI inside its frame; capturing mouse and keyboard from the game's input layer and suppressing them while the overlay is open; the conditions under which the overlay is available at all.
- `hook-dll/item-search`: Reading the client's database from inside the game — connection policy, the search query and its filter model, paging, and detecting that the client has changed the data underneath.
- `hook-dll/item-presentation`: Turning a database row into something a player can read — icon resolution, the game's own stat text rows with their colour codes and section types, and merging identical items into one stack.
- `hook-dll/in-game-transfer`: Moving an item from the overlay into the transfer stash, and handing the resulting deletion back to the client durably enough to survive a crash.

### Modified Capabilities

- `hook-dll/injection-lifecycle`: Hook installation currently assumes every hook can be installed whenever the game is ready. The overlay's hooks additionally depend on which renderer the game loaded, so the attach sequence must be able to install a subset and continue rather than treating an unavailable renderer as a failed attach.

## Impact

**New code in `HookDll/Hook`** — overlay host (device hook, input hooks, RmlUi interfaces), search (SQLite wrapper, query builder, result model), presentation (icon cache, replica row parser), transfer (item creation, journal writer).

**New third-party dependencies** — RmlUi (with its upstream `RmlUi_Renderer_DX11` backend), FreeType, the SQLite amalgamation, and a PNG decoder. All are new to the DLL.

**Build system** — `HookDll/Hook/Custom.vcxproj` is a v140 project carrying absolute paths to a specific boost and MSVC install. It must be able to consume a CMake-built static library before any of the above can be added. This is a prerequisite, not a side effect.

**Ported logic that now exists twice** — the search query in `IAGrim/Database/DAO/PlayerItemDaoImpl.cs`, the icon scoring in `DatabaseItemStatDaoImpl`, the stat-field tables in `IAGrim/UI/Filters/*.cs`, and the replica row section map in `WebUI/src/components/Item/replicaSections.ts` all gain a C++ counterpart that must be kept in step with the C# original.

**Existing behaviour touched** — `HookDll/Hook/dllmain.cpp` (hook registration and lifecycle), and the client gains a consumer for the transfer journal.

**Dead code to retire** — `HookDll/Shared/SQLite.cpp` and its `IDB`/`ITable` interfaces are in no solution, vendor no `sqlite3.h`, and predate this project. They are superseded rather than reused.
