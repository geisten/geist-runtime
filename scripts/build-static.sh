#!/bin/sh
# build-static.sh — a fully static Linux geistr (musl, libcurl, OpenSSL and
# libgomp linked in): runs on any Linux of its architecture, glibc or not.
# Run it in Alpine, as the release workflow does:
#   docker run --rm -v "$PWD:/src" -w /src alpine:3.21 sh scripts/build-static.sh
# Output: build/static/geistr (stripped).
set -eu
apk add -q build-base python3 git linux-headers curl-dev curl-static openssl-libs-static \
    nghttp2-static zlib-static brotli-static zstd-static libidn2-static libpsl-static \
    libunistring-static c-ares-dev
# Alpine's curl-config --static-libs leaves out brotlicommon and unistring.
make geistr BUILD=build/static LDFLAGS=-static \
    GEISTR_LIBS="$(curl-config --static-libs) -lbrotlicommon -lunistring"
strip build/static/geistr
build/static/geistr --version
# Built as root in a container: hand the files back to the checkout's owner.
chown -R "$(stat -c %u:%g .)" build geistlib 2>/dev/null || true
