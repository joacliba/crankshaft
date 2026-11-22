#!/bin/bash
# Builds the boot splash and the dashboard wallpapers (runs on the CI host, needs ImageMagick).
# usage: make-images.sh <out-dir> [logo.png]
set -euo pipefail
OUT=$1
LOGO=${2:-}
NAME='Cotxe de Climent'
mkdir -p "$OUT"
FONT=$(fc-match -f '%{file}' 'Inter:style=Light')

# Splash: fills the top 80% of the screen (moderno-splash scales it to fit), black like the screen.
if [ -n "$LOGO" ] && [ -f "$LOGO" ]; then
  # Light background around the logo becomes transparent (flood fill from the corner, the
  # logo's dark outline stops it), so it sits on black.
  convert "$LOGO" -alpha set -bordercolor white -border 1 -fuzz 12% -fill none -floodfill +0+0 white \
    -shave 1x1 -trim +repage "$OUT/logo-clean.png"
  convert -size 1600x768 xc:black \
    \( "$OUT/logo-clean.png" -resize 560x400 \) -gravity center -geometry +0-90 -composite \
    -font "$FONT" -pointsize 84 -kerning 6 -fill '#EDEFF3' -gravity center -annotate +0+250 "$NAME" \
    "$OUT/splash.png"
else
  convert -size 1600x768 xc:black \
    -font "$FONT" -pointsize 110 -kerning 8 -fill '#EDEFF3' -gravity center -annotate +0+0 "$NAME" \
    "$OUT/splash.png"
fi

# Wallpapers: graphite gradient with a soft blue light under the tiles (day) / dimmer (night).
wallpaper() { # top bottom glow-alpha file
  convert -size 1280x800 gradient:"$1"-"$2" \
    \( -size 1280x500 radial-gradient:"rgba(70,125,210,$3)"-"rgba(0,0,0,0)" \) \
    -gravity south -geometry +0-120 -composite "$4"
}
wallpaper '#1b1f26' '#06070a' 0.38 "$OUT/wallpaper.png"
wallpaper '#0e1014' '#030304' 0.20 "$OUT/wallpaper-night.png"
ls -la "$OUT"
