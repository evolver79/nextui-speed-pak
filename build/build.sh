#!/bin/sh
# Builds build/out/speed for the TrimUI Brick (tg5040) in an ARM64 Debian bullseye container. The first run also builds
# mbedTLS (build/deps.sh, a minute); later runs reuse it. SDL2 and SDL2_ttf come from the Brick.
set -e
cd "$(dirname "$0")/.."
sh build/fetch.sh
mkdir -p build/out
docker build -q -t nextui-speed-build build >/dev/null
docker run --rm -v "$PWD:/w" -w /w nextui-speed-build sh -c "
set -e
sh build/deps.sh >build/out/deps.log 2>&1 || { tail -40 build/out/deps.log; exit 1; }
gcc -O2 -Wall -o build/out/speed src/*.c ui/dot/src/dot.c -Iui/dot/src -Ibuild/out/prefix/include \
  \$(pkg-config --cflags sdl2) -Lbuild/out/prefix/lib -lmbedtls -lmbedx509 -lmbedcrypto \
  \$(pkg-config --libs sdl2) -lSDL2_ttf -lpthread -lm
strip build/out/speed
ls -la build/out/speed
"
