# Third-party dependencies for the hook DLL

RmlUi (the in-game overlay's UI toolkit) and FreeType (its font engine). Neither the
sources nor the built libraries are committed — see `.gitignore` for why — so a fresh
clone needs one run of `build_deps.cmd` before `Custom.vcxproj` will link.

## Getting the sources

```
git clone --depth 1 --branch 6.3          https://github.com/mikke89/RmlUi.git RmlUi
git clone --depth 1 --branch VER-2-14-1   https://gitlab.freedesktop.org/freetype/freetype.git freetype
```

| | Version | Licence |
|---|---|---|
| RmlUi | 6.3 | MIT |
| FreeType | 2.14.1 (`VER-2-14-1`) | FTL or GPLv2 |

## Building

```
build_deps.cmd
```

Produces `build\prefix\{include,lib,backends}`, which `HookDll\Directory.Build.props`
points at via `RmlUiPrefixDir`.

`backends` is staged by the script rather than by CMake. RmlUi treats its renderer and
platform backends as sample code that each application copies, so `--target install` does
not carry them; the hook compiles `RmlUi_Renderer_DX11.cpp` and `RmlUi_Platform_Win32.cpp`
itself. They are used unmodified and adapted by subclassing in `HookDll\Hook\OverlayUi.cpp`.
A `build\prefix` produced before that step existed will be missing them — re-run the script.

## Two settings that must not change

**`-T v143`.** The hook's x64 configurations build with v143. A static library built by
newer build tools can only be linked by an equally new linker, so building these with the
VS2026 (v145) default produces libraries the hook cannot link. This is also why the
upstream prebuilt `RmlUi-vs2026-win64` package is not used.

**`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`.** The hook is `/MD` because Grim Dawn
fills hook-allocated STL containers (see `VTableDispatch::Call`), so the game and the hook
must share one CRT heap. Every static library linked into the hook has to agree. A `/MT`
dependency does not fail to link — it corrupts the heap at runtime, which is far worse.

## Why RmlUi at all

Its public headers require C++17 (`Core/Traits.h` uses `if constexpr` and `std::is_same_v`),
which is what moved the hook's x64 configurations off v140. See the change's `design.md`.
