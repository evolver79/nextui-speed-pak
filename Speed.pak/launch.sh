#!/bin/sh
# Speed: how fast the Brick's Wi-Fi really is. Results and settings (speed.txt) are kept in NextUI's user data folder.
cd "$(dirname "$0")"
mkdir -p "$USERDATA_PATH/Speed"
./speed "$USERDATA_PATH/Speed" "$PWD" > "$LOGS_PATH/Speed.txt" 2>&1
