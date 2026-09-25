#!/bin/bash
# Grim Dawn - classic box64 + real x86_64 Wine (WOW64).
# Same pattern as launch-witcher3-box64.sh / launch-poe2-box64.sh: bypasses Hangover's
# FEX/ARM64EC path. The Lutris prefix at Games/gog/grim-dawn is NOT used as a prefix -
# its system32 DLLs are ARM64 PEs (kernel32.dll is Aarch64), unloadable by an x86_64
# process. A clean x86_64 prefix is used instead, with only the game FILES read from the
# GOG install directory. Only the 64-bit build (x64/Grim Dawn.exe) is used - box64 runs
# x86_64, not the 32-bit root Grim Dawn.exe.

BOX64="/home/aguqz/box64-src/build/box64"

export WINE="/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/bin/wine"
export WINEPREFIX="/home/aguqz/Games/grim-dawn-classic-box64"
export WINEARCH=win64
export DISPLAY="${DISPLAY:-:0}"

# Strict isolation: keep box64 off the system ARM64/Hangover packages in /usr/lib.
export BOX64_PATH="/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/bin"
export BOX64_LD_LIBRARY_PATH="/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/lib:/home/aguqz/wine-classic-box64/wine-11.16-amd64-wow64/lib64"

# --- box64 dynarec tuning ---
# STRONGMEM=2 : strongest x86 memory-ordering emulation (fixes threading/race glitches).
# WAIT=1      : wait for a dynarec block to finish compiling instead of racing ahead -
#               avoids the input-thread stall/deadlock when Sunshine injects virtual
#               controller state (symptom: freeze on area transitions with the Xbox pad,
#               unfreezing when the mouse nudges the message pump).
# FASTNAN=0   : IEEE-correct NaN handling.
export BOX64_DYNAREC_STRONGMEM=1
export BOX64_DYNAREC_WAIT=1
export BOX64_DYNAREC_FASTNAN=1
export BOX64_DYNAREC_BIGBLOCK=2

# --- Wine input/window fix ---
# Keep Wine from letting the D3D render window go idle on WM_CLOSE handling, which is what
# lets the input thread fall asleep under Sunshine's virtual controller injection.
export WINE_D3D_NO_WM_CLOSE=1

# Grim Dawn is DX11/DX9. Force native DXVK over Wine's builtin wined3d.
export WINEDLLOVERRIDES="d3d11,d3d10core,dxgi,d3d9=n"
export DXVK_STATE_CACHE_PATH="/home/aguqz/Games/grim-dawn-classic-box64/dxvk-cache"
export DXVK_CONFIG_FILE="/home/aguqz/Games/grim-dawn-classic-box64/dxvk.conf"
export DXVK_LOG_PATH="/home/aguqz/Games/grim-dawn-classic-box64/dxvk-logs"
export DXVK_LOG_LEVEL=info

# Free the GPU from inference (ds4-server) and clear stale Grim Dawn/wineserver locks.
[ -x /usr/local/bin/check-inference-before-grimdawn.sh ] && /usr/local/bin/check-inference-before-grimdawn.sh

# --- Item Assistant (IAGD) ---------------------------------------------------
# IA lives in THIS prefix (C:\Program Files\IAGD) because it injects
# ItemAssistantHook_x64.dll into the game and reads the shared stash - a separate
# prefix would be blind to both. It polls for the game process, so starting it
# first is fine. Single-instance: only launch it if it isn't already up.
# Kept off the game's cores (5-9,15-19) so the parser/UI can't steal game time.
# launch-iagd-dev.sh runs the forked build (native WinForms item grid, since WebView2 cannot
# present under Wine). Swap back to launch-iagd-box64.sh for the stock upstream install.
if [ -x /home/aguqz/.local/bin/launch-iagd-dev.sh ] && ! pgrep -f 'IAGrim\.exe' >/dev/null; then
    taskset -c 0,1,2,3 nohup /home/aguqz/.local/bin/launch-iagd-dev.sh \
        >/tmp/iagd-autostart.log 2>&1 &
fi

# --- optional debug overrides ------------------------------------------------
# Sourced last so it can override anything above. Create the file to run an experiment,
# DELETE IT to go back to normal - there is nothing to undo in this script.
DEBUG_ENV="${XDG_CONFIG_HOME:-$HOME/.config}/grim-dawn-debug.env"
if [ -r "$DEBUG_ENV" ]; then
    # shellcheck disable=SC1090
    . "$DEBUG_ENV"
    echo "launch-grimdawn: debug overrides from $DEBUG_ENV" >&2
fi

GAMEDIR="/home/aguqz/Games/gog/grim-dawn/drive_c/GOG Games/Grim Dawn"
cd "$GAMEDIR" || exit 1

# 64-bit build; run with the game root as CWD (Grim Dawn loads data relative to it).
exec taskset -c 5,6,7,8,9,15,16,17,18,19 "$BOX64" "$WINE" "x64/Grim Dawn.exe" "$@"
