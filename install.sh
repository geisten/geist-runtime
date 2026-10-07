#!/bin/sh
# install.sh — geistr for this computer from a GitHub release:
#   curl -fsSL https://raw.githubusercontent.com/geisten/geist-runtime/main/install.sh | sh
# GEISTR_VERSION=v0.1.0 picks a release (default: the latest);
# PREFIX=~/.local installs to ~/.local/bin (default: /usr/local, with sudo if needed).
set -eu
repo=geisten/geist-runtime
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)               target=linux-amd64 ;;
    Linux-aarch64 | Linux-arm64) target=linux-arm64 ;;
    Darwin-arm64)               target=macos-arm64 ;;
    *)
        echo "geistr: no release for $(uname -s) $(uname -m); build it: https://github.com/$repo#install" >&2
        exit 1
        ;;
esac
version=${GEISTR_VERSION:-latest}
if [ -n "${GEISTR_BASE_URL:-}" ]; then # tests: a local server
    base=$GEISTR_BASE_URL
elif [ "$version" = latest ]; then
    base=https://github.com/$repo/releases/latest/download
else
    base=https://github.com/$repo/releases/download/$version
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
echo "downloading geistr-$target ($version)"
curl -fsSL "$base/geistr-$target.tar.gz" -o "$tmp/geistr-$target.tar.gz"
curl -fsSL "$base/SHA256SUMS" -o "$tmp/SHA256SUMS"
expected=$(grep " geistr-$target.tar.gz\$" "$tmp/SHA256SUMS" | cut -d' ' -f1)
if command -v sha256sum >/dev/null; then
    actual=$(sha256sum "$tmp/geistr-$target.tar.gz" | cut -d' ' -f1)
else
    actual=$(shasum -a 256 "$tmp/geistr-$target.tar.gz" | cut -d' ' -f1)
fi
if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
    echo "geistr: the download does not match SHA256SUMS; nothing installed" >&2
    exit 1
fi
tar -xzf "$tmp/geistr-$target.tar.gz" -C "$tmp"
bin=${PREFIX:-/usr/local}/bin
if mkdir -p "$bin" 2>/dev/null && [ -w "$bin" ]; then
    install -m 755 "$tmp/geistr" "$bin/geistr"
else
    echo "installing to $bin (sudo)"
    sudo mkdir -p "$bin"
    sudo install -m 755 "$tmp/geistr" "$bin/geistr"
fi
"$bin/geistr" --version
case ":$PATH:" in *":$bin:"*) ;; *) echo "note: $bin is not on your PATH" ;; esac
