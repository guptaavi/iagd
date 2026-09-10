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

rem The two backend files the hook compiles itself. RmlUi's CMake install stages only the
rem library and its public headers; the backends are sample code that each application is
rem expected to take a copy of. Staging them next to the installed headers keeps the hook
rem project pointing at one reproducible location (build\prefix) instead of reaching into
rem the gitignored clone. They are used unmodified -- see HookDll\Hook\OverlayUi.cpp for
rem the subclassing that adapts them to a hook rather than an application.
echo === Backends ===
if not exist "%HERE%build\prefix\backends" mkdir "%HERE%build\prefix\backends"
for %%F in (RmlUi_Renderer_DX11.cpp RmlUi_Renderer_DX11.h RmlUi_Platform_Win32.cpp RmlUi_Platform_Win32.h RmlUi_Include_Windows.h) do (
  copy /Y "%HERE%RmlUi\Backends\%%F" "%HERE%build\prefix\backends\%%F" >nul
  if errorlevel 1 exit /b 1
)

echo.
echo === Built into %HERE%build\prefix ===
dir /b "%HERE%build\prefix\lib"
endlocal
