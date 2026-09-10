@echo off
rem Builds the offline tests for the overlay's database and presentation layers.
rem
rem These deliberately do not need the game, the hook, or the v143 toolset: those layers
rem have no game or logging dependencies so they can be exercised against a copy of a real
rem userdata database, which is how the ported query and the ported tooltip parsing are
rem checked against their C# and TypeScript originals.
rem
rem Usage:  build_tests.cmd          (from a Visual Studio developer command prompt)
rem   then, with DB="%%LOCALAPPDATA%%\EvilSoft\IAGD\data\userdata-test.db":
rem         ..\..\out\sqlite_query_test.exe  %%DB%%
rem         ..\..\out\item_search_test.exe   %%DB%% fixtures\csharp_default_search.sql
rem         ..\..\out\replica_text_test.exe  %%DB%%

setlocal
set HERE=%~dp0
set OUT=%HERE%..\..\out
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\objq" mkdir "%OUT%\objq"
if not exist "%OUT%\objs" mkdir "%OUT%\objs"
if not exist "%OUT%\objr" mkdir "%OUT%\objr"
if not exist "%OUT%\obji" mkdir "%OUT%\obji"

where cl.exe >nul 2>&1
if errorlevel 1 (
  echo cl.exe not found. Run this from a Visual Studio developer command prompt,
  echo or run vcvars64.bat first.
  exit /b 1
)

rem Must match SqliteCompileDefinitions in ..\Directory.Build.props.
set SQLITE_DEFS=/DSQLITE_THREADSAFE=1 /DSQLITE_OMIT_LOAD_EXTENSION /DSQLITE_DQS=0 ^
 /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_OMIT_DEPRECATED /DSQLITE_OMIT_PROGRESS_CALLBACK

set CLFLAGS=/nologo /W3 /O2 /MT /EHsc /std:c++17
set INCLUDES=/I "%HERE%..\sqlite" /I "%HERE%..\Hook"
set SQLITE_SRC="%HERE%..\sqlite\sqlite3.c"

rem Forward slashes on /Fo on purpose: a trailing backslash before the end of a batch line
rem swallows the newline and merges the next command into the compiler's arguments.
cl %CLFLAGS% %INCLUDES% %SQLITE_DEFS% ^
   "%HERE%sqlite_query_test.cpp" "%HERE%..\Hook\SqliteDb.cpp" %SQLITE_SRC% ^
   /Fe:"%OUT%\sqlite_query_test.exe" /Fo"%OUT%/objq/"
if errorlevel 1 exit /b 1

cl %CLFLAGS% %INCLUDES% %SQLITE_DEFS% ^
   "%HERE%item_search_test.cpp" "%HERE%..\Hook\ItemSearch.cpp" "%HERE%..\Hook\SqliteDb.cpp" %SQLITE_SRC% ^
   /Fe:"%OUT%\item_search_test.exe" /Fo"%OUT%/objs/"
if errorlevel 1 exit /b 1

cl %CLFLAGS% %INCLUDES% %SQLITE_DEFS% ^
   "%HERE%replica_text_test.cpp" "%HERE%..\Hook\ReplicaText.cpp" "%HERE%..\Hook\SqliteDb.cpp" %SQLITE_SRC% ^
   /Fe:"%OUT%\replica_text_test.exe" /Fo"%OUT%/objr/"
if errorlevel 1 exit /b 1

cl %CLFLAGS% %INCLUDES% %SQLITE_DEFS% ^
   "%HERE%item_icons_test.cpp" "%HERE%..\Hook\ItemIcons.cpp" "%HERE%..\Hook\SqliteDb.cpp" %SQLITE_SRC% ^
   /Fe:"%OUT%\item_icons_test.exe" /Fo"%OUT%/obji/"
if errorlevel 1 exit /b 1

echo Built sqlite_query_test.exe, item_search_test.exe, replica_text_test.exe and item_icons_test.exe
endlocal
