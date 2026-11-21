#!/bin/sh
# In the build container: mbedTLS as static libraries into build/out/prefix (once), with pthread support: the test
# runs four TLS connections from four threads, and they share one random generator and PSA's key store.
set -e
P=/w/build/out/prefix
[ -f $P/.threading ] && exit 0
rm -rf $P; mkdir -p /tmp/src $P && cd /tmp/src
tar xjf /w/deps/mbedtls-3.6.2.tar.bz2 && cd mbedtls-3.6.2
sed -i -e 's|^//#define MBEDTLS_THREADING_C$|#define MBEDTLS_THREADING_C|' \
       -e 's|^//#define MBEDTLS_THREADING_PTHREAD$|#define MBEDTLS_THREADING_PTHREAD|' include/mbedtls/mbedtls_config.h
grep -q '^#define MBEDTLS_THREADING_PTHREAD$' include/mbedtls/mbedtls_config.h
make -j4 lib CFLAGS="-O2" >/dev/null
make install DESTDIR=$P >/dev/null
touch $P/.threading
