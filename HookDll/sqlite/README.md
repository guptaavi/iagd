# SQLite (vendored)

The SQLite amalgamation, used by the hook DLL to read the Item Assistant client's
`userdata.db` from inside the game process.

| | |
|---|---|
| Version | 3.53.4 (`sqlite-amalgamation-3530400`) |
| Source | <https://sqlite.org/2026/sqlite-amalgamation-3530400.zip> |
| Archive size | 2,946,650 bytes |
| Archive SHA-256 | `1E71DDF93849C6A6ECF58B827C0692073D2DD7EE40196158068F7B29F422E87D` |
| Archive SHA3-256 | `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e` (as published on sqlite.org/download.html) |
| Licence | Public domain |

`shell.c` from the same archive is deliberately not vendored; it is the command-line
tool, not part of the library.

## Updating

Download the amalgamation from sqlite.org, check the size and hash against the figures
published on the download page, and replace `sqlite3.c`, `sqlite3.h` and `sqlite3ext.h`.
Update the table above. Do not edit the vendored sources; build options belong in
`HookDll/Directory.Build.props` so they survive an update.

## Build options

Set in `HookDll/Directory.Build.props` as `SqliteCompileDefinitions`, and applied only
to `sqlite3.c`:

- `SQLITE_THREADSAFE=1` — the hook queries from a worker thread while the game's
  threads may also hold the connection open. Serialized mode is the safe default here.
- `SQLITE_OMIT_LOAD_EXTENSION` — nothing loads extensions, and this removes the
  ability to load arbitrary code from a database file.
- `SQLITE_DQS=0` — double-quoted string literals are rejected rather than silently
  treated as strings, so a mistyped identifier in a ported query fails loudly.
- `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_OMIT_DEPRECATED`, `SQLITE_OMIT_PROGRESS_CALLBACK` —
  trim code the hook does not use out of a DLL that is injected into a game.

`sqlite3.c` is C, not C++, and must be compiled with the precompiled header disabled;
the hook's PCH (`stdafx.h`) is C++.
