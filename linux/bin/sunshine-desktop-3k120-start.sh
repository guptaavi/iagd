#!/bin/bash
export DISPLAY=:0
# 691.20MHz is the 3840x1080_60 timing at double the pixel clock, so nothing but the refresh
# changes. Well inside what this driver runs - the 5K mode on the same output is 1324MHz - and
# there is no physical link to saturate, the display being virtual.
/usr/local/bin/set-virtual-display-mode.sh "3840x1080_120.00" \
    691.20 3840 4080 4496 5152 1080 1081 1084 1118 -HSync +Vsync
exec /usr/local/bin/sunshine-xfce-start.sh
