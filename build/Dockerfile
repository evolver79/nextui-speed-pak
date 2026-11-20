# Debian bullseye arm64: glibc 2.31, none newer than the TrimUI Brick's. mbedTLS is built from source as a static
# library, so the pak needs only what the Brick already has (SDL2, SDL2_ttf, libc).
FROM --platform=linux/arm64 debian:bullseye
RUN printf '%s\n' "deb http://archive.debian.org/debian bullseye main" \
      "deb http://archive.debian.org/debian bullseye-updates main" \
      "deb http://archive.debian.org/debian-security bullseye-security main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends gcc make pkg-config bzip2 python3 ffmpeg ca-certificates \
      libsdl2-dev libsdl2-ttf-dev
