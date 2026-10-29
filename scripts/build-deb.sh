#!/bin/sh
# build-deb.sh — a .deb of the Linux geistr (Vulkan build) for Ubuntu and Debian:
# the binary, the GNOME launcher (geistr chat in a terminal), the icon and the
# systemd user service geistr (packaging/linux/).
#   scripts/build-deb.sh build/vulkan/geistr 0.4.0 amd64 dist
# Output: <outdir>/geistr_<version>_<arch>.deb
set -eu
bin=$1 version=$2 arch=$3 out=$4
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT INT TERM
chmod 755 "$root"
install -Dm755 "$bin" "$root/usr/bin/geistr"
install -Dm644 "$here/packaging/linux/net.geisten.geistr.desktop" "$root/usr/share/applications/net.geisten.geistr.desktop"
install -Dm644 "$here/packaging/linux/net.geisten.geistr.svg" "$root/usr/share/icons/hicolor/scalable/apps/net.geisten.geistr.svg"
install -Dm644 "$here/packaging/linux/geistr.service" "$root/usr/lib/systemd/user/geistr.service"
install -Dm644 "$here/LICENSE" "$root/usr/share/doc/geistr/copyright"
install -Dm644 "$here/THIRD_PARTY_LICENSES" "$root/usr/share/doc/geistr/THIRD_PARTY_LICENSES"
mkdir -p "$root/DEBIAN"
# build-vulkan.sh links only glibc and libgomp; libvulkan1 for the GPU (#253).
# pdftotext (poppler-utils) reads PDFs for geistr index.
cat > "$root/DEBIAN/control" <<CONTROL
Package: geistr
Version: $version
Architecture: $arch
Maintainer: Germar Schlegel <g.schlegel@geisten.com>
Installed-Size: $(du -sk "$root/usr" | cut -f1)
Section: utils
Priority: optional
Depends: libc6 (>= 2.28), libgomp1
Recommends: libvulkan1, poppler-utils
Homepage: https://github.com/geisten/geist-runtime
Description: run language models on your own computer
 Download, chat, serve: one small binary, no Python, no account.
 The model runs locally, on the CPU or the GPU (Vulkan). Includes a GNOME
 launcher and the systemd user service geistr for the OpenAI and
 Ollama API on 127.0.0.1:11434.
CONTROL
mkdir -p "$out"
dpkg-deb --root-owner-group --build "$root" "$out/geistr_${version}_$arch.deb"
