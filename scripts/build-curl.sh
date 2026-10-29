#!/bin/sh
# build-curl.sh [configure args…] — the minimal static libcurl of the Linux
# releases into /usr/local (HTTPS only; no IDN, PSL or compression libraries:
# those would bring LGPL code into a static binary). For build-static.sh and
# build-vulkan.sh; extra arguments go to configure (e.g. --with-openssl=DIR).
set -eu
CURL=curl-8.22.0
CURL_SHA256=d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1
cd /tmp
curl -fsSLO "https://curl.se/download/$CURL.tar.gz" 2>/dev/null || wget -q "https://curl.se/download/$CURL.tar.gz"
echo "$CURL_SHA256  $CURL.tar.gz" | sha256sum -c -
tar xzf "$CURL.tar.gz"
cd "$CURL"
./configure -q --prefix=/usr/local --disable-shared --enable-static --with-openssl \
    --without-libidn2 --without-libpsl --without-brotli --without-zstd --without-nghttp2 \
    --without-zlib --without-libssh2 --disable-ldap --disable-docs --disable-manual \
    --disable-dict --disable-file --disable-ftp --disable-gopher --disable-imap --disable-mqtt \
    --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet --disable-tftp "$@"
make -s -j"$(nproc)"
make -s install
