@echo off
rem Builds the SQLite smoke test against the vendored amalgamation.
rem
rem Standalone on purpose: it verifies the database side of the hook without a game,
rem an injection, or the v140 toolset the DLL itself is pinned to.
rem
rem Usage:  build_sqlite_smoke.cmd
rem   then: ..\..\out\sqlite_smoke.exe "%%LOCALAPPDATA%%\EvilSoft\IAGD\data\userdata.db"

setlocal
set HERE=%~dp0
set OUT=%HERE%..\..\out
if not exist "%OUT%" mkdir "%OUT%"

where cl.exe >nul 2>&1
if errorlevel 1 (
  echo cl.exe not found. Run this from a Visual Studio developer command prompt,
  echo or run VsDevCmd.bat / vcvars64.bat first.
  exit /b 1
)

cl /nologo /W3 /O2 /MT ^
   /I "%HERE%..\sqlite" ^
   /DSQLITE_THREADSAFE=1 /DSQLITE_OMIT_LOAD_EXTENSION /DSQLITE_DQS=0 ^
   /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_OMIT_DEPRECATED /DSQLITE_OMIT_PROGRESS_CALLBACK ^
   "%HERE%sqlite_smoke.c" "%HERE%..\sqlite\sqlite3.c" ^
   /Fe:"%OUT%\sqlite_smoke.exe" /Fo:"%OUT%\\"
if errorlevel 1 exit /b 1

echo Built %OUT%\sqlite_smoke.exe
endlocal
