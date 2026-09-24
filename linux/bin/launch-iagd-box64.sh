#!/bin/bash
# Grim Dawn Item Assistant (IAGD) - same box64 + x86_64-Wine stack and PREFIX as the game.
# IA must live in Grim Dawn's own prefix or it cannot see the stash or inject its hook.
# Game launcher: /usr/local/bin/launch-grimdawn-box64.sh
#
# Two Wine-specific workarounds live here:
#
# 1. CWD. IAGrim resolves hibernate.sqlite.cfg.xml RELATIVE TO THE WORKING DIRECTORY, so it
#    must start with its install folder as CWD. Launched from anywhere else it dies on startup
#    with FileNotFoundException 'hibernate.sqlite.cfg.xml' -> NHibernate HibernateConfigException.
#
# 2. Blank item grid. Under Wine, WebView2's navigation to https://app/index.html intermittently
#    never completes (upstream documents this in IAGrim/Utilities/WebView2Runtime.cs and already
#    passes --no-sandbox --disable-gpu --single-process to mitigate it). When it stalls, the
#    WinForms shell still works - filters, status bar, loot capture, the hook - but the item grid
#    stays empty because every SetItems message sits queued behind a UI that never signals ready.
#    The stall is a startup race, so a restart clears it. We detect it by watching IA's own log
#    for "UI signalled readiness" and relaunch if it doesn't arrive.

IAGD_DIR="/home/aguqz/Games/grim-dawn-classic-box64/drive_c/Program Files/IAGD"
IAGD_LOG="/home/aguqz/Games/grim-dawn-classic-box64/drive_c/users/aguqz/AppData/Local/EvilSoft/IAGD/log.txt"
READY_TIMEOUT=60   # seconds to wait for the browser UI to signal readiness
MAX_ATTEMPTS=3

cd "$IAGD_DIR" || exit 1

# --diagnose and friends: pass straight through, no readiness watchdog.
if [ "$#" -gt 0 ]; then
    exec /home/aguqz/.local/bin/gd-box64-wine.sh "C:\\Program Files\\IAGD\\IAGrim.exe" "$@"
fi

for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
    /home/aguqz/.local/bin/gd-box64-wine.sh "C:\\Program Files\\IAGD\\IAGrim.exe" &
    launcher_pid=$!

    # IA rotates log.txt on every start, so a match here always belongs to this run.
    ready=0
    for _ in $(seq 1 "$READY_TIMEOUT"); do
        sleep 1
        if grep -q "UI signalled readiness" "$IAGD_LOG" 2>/dev/null; then
            ready=1
            break
        fi
        kill -0 "$launcher_pid" 2>/dev/null || break
    done

    if [ "$ready" = 1 ]; then
        wait "$launcher_pid"
        exit $?
    fi

    # Browser never came up. Kill this instance and try once more.
    echo "iagd: item grid never signalled ready (attempt $attempt/$MAX_ATTEMPTS), restarting" >&2
    pkill -f 'IAGD\\IAGrim.exe' 2>/dev/null
    kill "$launcher_pid" 2>/dev/null
    sleep 5
done

echo "iagd: giving up after $MAX_ATTEMPTS attempts; starting once more without the watchdog" >&2
exec /home/aguqz/.local/bin/gd-box64-wine.sh "C:\\Program Files\\IAGD\\IAGrim.exe"
