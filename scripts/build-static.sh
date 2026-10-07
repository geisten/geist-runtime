#!/bin/sh
# build-static.sh — a fully static Linux geistr: musl, OpenSSL, libgomp and a
# minimal libcurl (HTTPS only, built here from its release; no IDN, PSL or
# compression libraries: those would bring LGPL code into a static binary).
# Runs on any Linux of its architecture, glibc or not. Run it in Alpine, as
# the release workflow does:
#   docker run --rm -v "$PWD:/src" -w /src alpine:3.21 sh scripts/build-static.sh
# Output: build/static/geistr (stripped). Licenses: THIRD_PARTY_LICENSES.
set -eu
CURL=curl-8.22.0
CURL_SHA256=d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1
apk add -q build-base python3 git linux-headers perl openssl-dev openssl-libs-static
(
    cd /tmp
    wget -q "https://curl.se/download/$CURL.tar.gz"
    echo "$CURL_SHA256  $CURL.tar.gz" | sha256sum -c -
    tar xzf "$CURL.tar.gz"
    cd "$CURL"
    ./configure -q --prefix=/usr/local --disable-shared --enable-static --with-openssl \
        --without-libidn2 --without-libpsl --without-brotli --without-zstd --without-nghttp2 \
        --without-zlib --without-libssh2 --disable-ldap --disable-docs --disable-manual \
        --disable-dict --disable-file --disable-ftp --disable-gopher --disable-imap --disable-mqtt \
        --disable-pop3 --disable-rtsp --disable-smb --disable-smtp --disable-telnet --disable-tftp
    make -s -j"$(nproc)"
    make -s install
)
make geistr BUILD=build/static LDFLAGS=-static GEISTR_LIBS="$(/usr/local/bin/curl-config --static-libs)"
strip build/static/geistr
build/static/geistr --version
# Built as root in a container: hand the files back to the checkout's owner.
chown -R "$(stat -c %u:%g .)" build geistlib 2>/dev/null || true
