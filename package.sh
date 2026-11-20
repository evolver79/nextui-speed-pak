#!/bin/sh
# Builds dist/Speed.pak.zip with the pak's files at the top of the zip (the layout Pak Store expects).
# For a manual install, unzip it into a new folder: SD card Tools/tg5040/Speed.pak/.
set -e
cd "$(dirname "$0")"
build/build.sh >/dev/null
cp build/out/speed Speed.pak/speed
rm -rf Speed.pak/fonts && cp -r ui/dot/fonts Speed.pak/fonts
cp deps/cacert.pem Speed.pak/cacert.pem
cp src/vendor/LICENSE-cJSON Speed.pak/
mkdir -p dist && rm -f dist/Speed.pak.zip
(cd Speed.pak && zip -q -X -r ../dist/Speed.pak.zip speed launch.sh fonts cacert.pem LICENSE-cJSON)
unzip -l dist/Speed.pak.zip
