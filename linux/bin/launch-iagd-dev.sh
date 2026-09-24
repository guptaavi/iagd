#!/bin/bash
# Development build of Item Assistant (fork with a native WinForms item grid).
# Same box64 + Wine stack and prefix as the game and the stock install; only the
# program folder differs, so it shares the same AppData data dir (items, settings).
# Do NOT run this at the same time as the stock IAGD - they fight over the same DB.
cd "/home/aguqz/Games/grim-dawn-classic-box64/drive_c/Program Files/IAGD-dev" || exit 1
export IAGD_UI_SCALE="${IAGD_UI_SCALE:-1.6}"
exec /home/aguqz/.local/bin/gd-box64-wine.sh "C:\\Program Files\\IAGD-dev\\IAGrim.exe" "$@"
