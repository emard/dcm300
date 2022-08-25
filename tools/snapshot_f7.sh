#!/bin/sh

# co /etc/xdg/openbox/lxde-pi-rc.xml ~/.config/openbox/lxde-pi-rc.xml
# joe /.config/openbox/lxde-pi-rc.xml
#    <keybind key="F7">
#      <action name="Execute">
#        <command>/home/davor/bin/snapshot_f7.sh</command>
#      </action>
#    </keybind>

~/bin/dcm300 \
| convert \
pnm:- \
-background white \
-pointsize 56 \
-font DejaVu-Sans-Mono \
label:'40x 0▕▄█▄█▄█▄█▄█ 100 µm' \
-gravity center \
-append \
/tmp/microscope.jpg

mv /tmp/microscope.jpg ~/Pictures/microscope.jpg
eog ~/Pictures/microscope.jpg
