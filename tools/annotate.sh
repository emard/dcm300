#!/bin/sh
convert \
/tmp/input.pnm \
-background white \
-pointsize 70 \
-font DejaVu-Sans-Mono \
label:'.   ▕          ▏\nmm 0▕░▒░▒░▒░▒░▒▏1 mm\n    ▕          ▏' \
-gravity center \
-append \
/tmp/output.pnm

