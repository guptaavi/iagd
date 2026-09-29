#!/bin/bash
export DISPLAY=:0
/usr/local/bin/set-virtual-display-mode.sh "3840x1080_60.00" \
    345.60 3840 4080 4496 5152 1080 1081 1084 1118 -HSync +Vsync
exec /usr/local/bin/sunshine-xfce-start.sh
