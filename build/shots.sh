#!/bin/sh
# Builds Speed, then renders every screen headless (SDL dummy video, software renderer) with made-up numbers into
# build/out/shots/*.png, to compare with the Dot style guide.
set -e
cd "$(dirname "$0")/.."
build/build.sh >/dev/null
O=build/out/shots
rm -rf $O && mkdir -p $O/user $O/pak
docker run --rm -v "$PWD:/w" -w /w -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy nextui-speed-build sh -c "
cp -r ui/dot/fonts $O/pak/ && build/out/speed $O/user $O/pak --shots $O >/dev/null
for f in $O/*.bmp; do ffmpeg -v error -y -i \"\$f\" \"\${f%.bmp}.png\" && rm \"\$f\"; done"
ls $O/*.png
