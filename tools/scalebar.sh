#!/bin/sh

# usage:
# ./scalebar.sh input.pnm output.pnm

# edit -pointsize value to adjust scale

convert \
$1 \
-background white \
-pointsize 70 \
-font DejaVu-Sans-Mono \
label:'mm 0▕▄█▄█▄█▄█▄█▏1 mm' \
-gravity center \
-append \
$2

