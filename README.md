# DCM300 Camera userspace driver

This driver is sensitive to linux realtime latency.
In case of problems (freezup) run it with realtime
execution priviledge with ulimit.

Usage:

Make a simple snapshot with default options:

    dcm300 > /tmp/image.pnm

Make a snapshot with non-default options:

    dcm300 -r 40 -g 40 -b 40 -e 200 > /tmp/image.pnm

To annotate image with a simple scale bar:

    tools/scalebar.sh /tmp/image.pnm /tmp/image-scalebar.pnm

To automate some actions with mouse and keyboard buttons/hotkeys,
use "Input remapper"

    apt install input-remapper