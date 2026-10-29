#!/bin/sh
# build-static.sh — a fully static Linux geistr: musl, OpenSSL, libgomp and a
# minimal libcurl (scripts/build-curl.sh). CPU only: a static binary cannot
# load the system's libvulkan; build-vulkan.sh makes the GPU build (#253).
# Runs on any Linux of its architecture, glibc or not. Run it in Alpine, as
# the release workflow does:
#   docker run --rm -v "$PWD:/src" -w /src alpine:3.21 sh scripts/build-static.sh
# Output: build/static/geistr (stripped). Licenses: THIRD_PARTY_LICENSES.
set -eu
apk add -q build-base python3 git linux-headers perl openssl-dev openssl-libs-static
sh scripts/build-curl.sh
# the engine keeps its objects across builds: none from build-vulkan.sh
rm -rf geistlib/build geistlib/lib
git config --global --add safe.directory "*"
# GEISTR_OPENSSL: model checksums through OpenSSL, which libcurl links anyway (#254)
make geistr BUILD=build/static CFLAGS="-O2 -g -DGEISTR_OPENSSL" LDFLAGS=-static GEISTR_LIBS="$(/usr/local/bin/curl-config --static-libs)"
strip build/static/geistr
build/static/geistr --version
# Built as root in a container: hand the files back to the checkout's owner.
chown -R "$(stat -c %u:%g .)" build geistlib 2>/dev/null || true
