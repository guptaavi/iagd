#!/bin/bash
export DISPLAY=:0
# Mode name plus the modeline to recreate it with, since --newmode modes do not survive an X restart.
/usr/local/bin/set-virtual-display-mode.sh "5120x1440_120.00" \
    1324.26 5120 5560 6136 7152 1440 1441 1444 1543 -HSync +Vsync
exec /usr/local/bin/sunshine-xfce-start.sh
