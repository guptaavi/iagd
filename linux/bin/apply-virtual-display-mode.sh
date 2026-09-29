#!/bin/bash
# Runs when the X server starts. Creates every mode the Moonlight desktop entries can ask for, and
# leaves the display on the default one.
#
# Creating modes is safe; SWITCHING between them is not. A switch on this driver has wedged the
# modeset path badly enough to need a reboot, so the boot default is set to whatever is used most.
# Every entry that matches it is then a no-op - the one path that has never failed.
#
# Modes added with --newmode are runtime-only and do not survive an X restart, which is why they
# are recreated here every time rather than assumed.
export DISPLAY=:0

for _ in $(seq 1 30); do
    timeout 5 xrandr --query >/dev/null 2>&1 && break
    sleep 1
done

# Create both, so neither desktop entry ever asks for a mode that does not exist.
timeout 5 xrandr --newmode "5120x1440_120.00" 1324.26 5120 5560 6136 7152 1440 1441 1444 1543 -HSync +Vsync 2>/dev/null
timeout 5 xrandr --addmode HDMI-0 "5120x1440_120.00" 2>/dev/null
timeout 5 xrandr --newmode "3840x1080_120.00" 691.20 3840 4080 4496 5152 1080 1081 1084 1118 -HSync +Vsync 2>/dev/null
timeout 5 xrandr --addmode HDMI-0 "3840x1080_120.00" 2>/dev/null
timeout 5 xrandr --newmode "3840x1080_60.00" 345.60 3840 4080 4496 5152 1080 1081 1084 1118 -HSync +Vsync 2>/dev/null
timeout 5 xrandr --addmode HDMI-0 "3840x1080_60.00" 2>/dev/null

# The default: 3840x1080 at 120Hz. 5K at 120 is 885 Mpx/s to capture, encode and ship, which was
# visibly choppy for Grim Dawn; 3K at 120 is 497 Mpx/s and smooth. Falls back rather than leaving
# the output unset, because this box is headless and has no console to fix it from.
/usr/local/bin/set-virtual-display-mode.sh "3840x1080_120.00" \
    691.20 3840 4080 4496 5152 1080 1081 1084 1118 -HSync +Vsync \
  || timeout 10 xrandr --output HDMI-0 --mode "3840x1080_60.00" 2>/dev/null

exit 0
