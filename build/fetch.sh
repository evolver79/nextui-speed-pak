#!/bin/sh
# Downloads the library sources build/deps.sh builds (into deps/, once) and checks them; and the CA list for HTTPS.
set -e
cd "$(dirname "$0")/../deps"
get() {   # file url sha256 (- = not checked: the CA list changes with every update of it)
  [ -f "$1" ] || { curl -fsSL -o "$1.part" "$2" && mv "$1.part" "$1"; }
  [ "$3" = - ] || echo "$3  $1" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: deps/$1" >&2; exit 1; }
}
get mbedtls-3.6.2.tar.bz2 https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
get cacert.pem https://curl.se/ca/cacert.pem -
