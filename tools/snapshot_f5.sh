#!/bin/sh

# co /etc/xdg/openbox/lxde-pi-rc.xml ~/.config/openbox/lxde-pi-rc.xml
# joe /.config/openbox/lxde-pi-rc.xml
#    <keybind key="F5">
#      <action name="Execute">
#        <command>/home/davor/bin/snapshot_f5.sh</command>
#      </action>
#    </keybind>

~/bin/dcm300 \
| convert \
pnm:- \
-background white \
-font DejaVu-Sans-Mono \
-pointsize 55 \
label:'4x 0▕▄█▄█▄█▄█▄█ 1 mm' \
-gravity center \
-append \
/tmp/microscope.jpg

mv /tmp/microscope.jpg ~/Pictures/microscope.jpg
eog ~/Pictures/microscope.jpg

# -pointsize 57 \
# label:'4x 0▕▄█▄█▄█▄█▄█ 2 mm' \
