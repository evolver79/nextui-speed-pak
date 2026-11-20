#!/bin/sh
# Builds Speed and copies it to the Brick with the brick tool (a private helper: IP and password in
# ~/.config/brick/config; any way of copying Speed.pak to SD card Tools/tg5040/ does the same).
set -e
cd "$(dirname "$0")"
brick ping >/dev/null
build/build.sh
cp build/out/speed Speed.pak/speed
rm -rf Speed.pak/fonts && cp -r ui/dot/fonts Speed.pak/fonts
cp deps/cacert.pem Speed.pak/cacert.pem
brick deploy Speed.pak >/dev/null
echo "Done. Open Tools > Speed on the Brick."
