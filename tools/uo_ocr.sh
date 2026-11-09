#!/usr/bin/env bash
# uo_ocr.sh <png>: the names in the battle map's unit panels (the left and right name boxes along
# the bottom of a 16:9 frame), read with tesseract. Expect misreads ("[lenia's Unit"): match a
# part of the name.
read -r w h < <(magick identify -format '%w %h' "$1")
for x in 0.123 0.681; do
    box=$(awk -v w="$w" -v h="$h" -v x="$x" 'BEGIN{printf "%dx%d+%d+%d", w*0.188, h*0.052, w*x, h*0.924}')
    magick "$1" -crop "$box" +repage -resize 400% -colorspace gray -negate png:- 2>/dev/null |
        tesseract stdin stdout --psm 7 2>/dev/null
done | tr '\n' ' '
