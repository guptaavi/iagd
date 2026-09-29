#!/bin/bash
# Set the headless virtual display's mode, safely.
#
# Usage: set-virtual-display-mode.sh <mode-name> [modeline args for --newmode...]
#
# Three hazards this exists to contain, each one observed on this box:
#
#  1. A mode CHANGE can wedge the NVIDIA modeset path. Switching from 3840x1080_60 into
#     5120x1440_120 killed a live stream - the desktop outline formed and then the stream died -
#     and an earlier switch left the X server dead-but-unreapable, still holding nvidia_modeset
#     references, recoverable only by rebooting. Setting the mode the display is ALREADY on is
#     harmless and has always worked, so that case returns early and touches nothing.
#
#  2. `xrandr --output --mode` HANGS when the modeset is wedged. The desktop start scripts called
#     it unguarded and then exec'd the session, so a hang meant the desktop never started at all -
#     which read as "the desktop is broken" when the display was the only thing wrong. Hence the
#     timeout, and hence this script always exiting 0.
#
#  3. Modes added with --newmode are runtime-only and disappear with the X server. 5120x1440_120
#     silently vanished once, which made the 5K entry fail on a mode that no longer existed. It is
#     (re)created here rather than assumed to be present.

set -u

export DISPLAY="${DISPLAY:-:0}"
OUTPUT="${VDISPLAY_OUTPUT:-HDMI-0}"

MODE="${1:?usage: set-virtual-display-mode.sh <mode-name> [modeline...]}"
shift

log() { echo "set-virtual-display-mode: $*" >&2; }

# X may still be coming up when this runs at session start; wait, but never forever.
for _ in $(seq 1 30); do
    timeout 5 xrandr --query >/dev/null 2>&1 && break
    sleep 1
done

current=$(timeout 5 xrandr 2>/dev/null | awk '/\*/ {print $1; exit}')

# The no-hazard path, and the common one: nothing to switch.
if [ "$current" = "$MODE" ]; then
    log "already on $MODE"
    exit 0
fi

# Recreate the mode if the server has forgotten it. Both calls fail harmlessly if it exists.
if [ "$#" -gt 0 ]; then
    timeout 5 xrandr --newmode "$MODE" "$@" 2>/dev/null
fi
timeout 5 xrandr --addmode "$OUTPUT" "$MODE" 2>/dev/null

if timeout 10 xrandr --output "$OUTPUT" --mode "$MODE" 2>/dev/null; then
    log "switched ${current:-unknown} -> $MODE"
    # Let the driver settle before anything starts capturing the new mode.
    sleep 1
else
    log "WARNING: could not switch to $MODE (still on ${current:-unknown}); starting the desktop anyway"
fi

# Never fail. A display problem must not be able to stop the desktop from coming up.
exit 0
