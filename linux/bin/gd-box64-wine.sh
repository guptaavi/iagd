#!/bin/bash
# Run an arbitrary Windows program inside the SAME box64 + x86_64-Wine stack and prefix
# that Grim Dawn itself uses (see /usr/local/bin/launch-grimdawn-box64.sh).
# Used for Item Assistant (IAGD) and its prerequisites: they must live in the game prefix,
# otherwise IA cannot see the stash or inject its hook.
BOX64="/home/aguqz/box64-src/build/box64"
export WINE="/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/bin/wine"
export WINEPREFIX="/home/aguqz/Games/grim-dawn-classic-box64"
export WINEARCH=win64
export DISPLAY="${DISPLAY:-:0}"
export BOX64_PATH="/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/bin"
export BOX64_LD_LIBRARY_PATH="/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/lib:/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/lib64"
export BOX64_DYNAREC_STRONGMEM=1
export BOX64_DYNAREC_WAIT=1
export BOX64_DYNAREC_FASTNAN=1
export BOX64_DYNAREC_BIGBLOCK=2
exec "$BOX64" "$WINE" "$@"
