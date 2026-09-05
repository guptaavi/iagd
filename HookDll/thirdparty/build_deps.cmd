@echo off
rem Builds RmlUi and FreeType as static libraries for the hook DLL, then stages the
rem headers and .lib files into HookDll\rmlui\ and HookDll\freetype\ (which are the
rem parts that get committed -- the source trees here are gitignored).
rem
rem Two settings below are not optional and will cause subtle breakage if changed:
rem
rem   -T v143   The hook's x64 configurations build with v143. A static library built
rem             by newer build tools can only be linked by an equally new linker, so
rem             building these with the VS2026 (v145) default would not link.
rem
rem   MultiThreadedDLL  The hook is /MD because the game fills hook-allocated STL
rem             containers, so both sides must share one CRT heap. Every static library
rem             linked into the hook has to agree, or the mismatch shows up as heap
rem             corruption at runtime rather than as a link error.
rem
rem Sources are pinned: RmlUi 6.3, FreeType VER-2-14-1. See clone commands in README.

setlocal
set HERE=%~dp0
set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set GEN=Visual Studio 17 2022
set COMMON=-G "%GEN%" -A x64 -T v143 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DBUILD_SHARED_LIBS=OFF

if not exist %CMAKE% (
  echo CMake not found at %CMAKE%
  exit /b 1
)

echo === FreeType ===
%CMAKE% -S "%HERE%freetype" -B "%HERE%build\freetype" %COMMON% ^
  -DCMAKE_INSTALL_PREFIX="%HERE%build\prefix" ^
  -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON
if errorlevel 1 exit /b 1
%CMAKE% --build "%HERE%build\freetype" --config Release --target install
if errorlevel 1 exit /b 1

echo === RmlUi ===
%CMAKE% -S "%HERE%RmlUi" -B "%HERE%build\rmlui" %COMMON% ^
  -DCMAKE_INSTALL_PREFIX="%HERE%build\prefix" ^
  -DCMAKE_PREFIX_PATH="%HERE%build\prefix" ^
  -DRMLUI_SAMPLES=OFF -DRMLUI_TESTS=OFF -DRMLUI_LUA_BINDINGS=OFF
if errorlevel 1 exit /b 1
%CMAKE% --build "%HERE%build\rmlui" --config Release --target install
if errorlevel 1 exit /b 1

echo.
echo === Built into %HERE%build\prefix ===
dir /b "%HERE%build\prefix\lib"
endlocal
