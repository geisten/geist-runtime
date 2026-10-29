#!/bin/sh
# build-vulkan.sh — the Linux geistr with the Vulkan GPU backend (#253): glibc
# 2.28 or newer, dynamically linked only against glibc and libgomp; OpenSSL and
# a minimal libcurl (build-curl.sh) are linked in. The engine dlopens
# libvulkan.so.1 when it runs; without one it is the CPU build. Run it in the
# manylinux_2_28 image (AlmaLinux 8, GCC 14), as the release workflow does:
#   docker run --rm -v "$PWD:/src" -w /src quay.io/pypa/manylinux_2_28_x86_64 sh scripts/build-vulkan.sh
# Output: build/vulkan/geistr (stripped). Licenses: THIRD_PARTY_LICENSES.
set -eu
OPENSSL=openssl-3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
dnf install -q -y vulkan-headers perl-IPC-Cmd perl-Time-Piece >/dev/null
(
    cd /tmp
    curl -fsSLO "https://github.com/openssl/openssl/releases/download/$OPENSSL/$OPENSSL.tar.gz"
    echo "$OPENSSL_SHA256  $OPENSSL.tar.gz" | sha256sum -c -
    tar xzf "$OPENSSL.tar.gz"
    cd "$OPENSSL"
    # OPENSSLDIR as on Debian; pull.c names the CA bundle itself
    ./Configure no-shared no-apps no-tests no-docs --prefix=/usr/local --libdir=lib --openssldir=/etc/ssl >/dev/null
    make -s -j"$(nproc)"
    make -s install_sw
)
sh scripts/build-curl.sh --with-openssl=/usr/local
# vulkan-headers: the Makefile adds the Vulkan backend. The engine keeps its
# objects across builds, and build-static.sh's have none: start it clean.
rm -rf geistlib/build geistlib/lib
git config --global --add safe.directory "*"
make geistr BUILD=build/vulkan CFLAGS="-O2 -g -DGEISTR_OPENSSL" \
    GEISTR_LIBS="$(/usr/local/bin/curl-config --static-libs)"
strip build/vulkan/geistr
build/vulkan/geistr --version
# Built as root in a container: hand the files back to the checkout's owner.
chown -R "$(stat -c %u:%g .)" build geistlib 2>/dev/null || true
