#!/bin/sh
# In the build container: mbedTLS as static libraries into build/out/prefix (once).
set -e
P=/w/build/out/prefix
[ -f $P/lib/libmbedtls.a ] && exit 0
mkdir -p /tmp/src $P && cd /tmp/src
tar xjf /w/deps/mbedtls-3.6.2.tar.bz2 && cd mbedtls-3.6.2
make -j4 lib CFLAGS="-O2" >/dev/null
make install DESTDIR=$P >/dev/null
