#!/bin/bash
export DISPLAY=:0
# 1920x1080 comes from the CustomEDID, so it needs no modeline.
/usr/local/bin/set-virtual-display-mode.sh "1920x1080"
exec /usr/local/bin/sunshine-xfce-start.sh
