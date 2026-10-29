#!/bin/sh
# install.sh — geistr for this computer from a release on www.geisten.net:
#   curl -fsSL https://www.geisten.net/download/geistr/install.sh | sh
# GEISTR_VERSION=v0.4.0 picks a release (default: the latest); the server keeps the
# last 5 releases (release.yml, KEEP), none before 0.3.2.
# PREFIX=~/.local installs to ~/.local/bin (default: /usr/local, with sudo if needed).
# On Linux with a Vulkan loader (libvulkan1) and libgomp it takes the GPU build
# (glibc 2.28 or newer), otherwise the static CPU build (#253).
set -eu
site=https://www.geisten.net/download/geistr

# Everything is one function, called on the last line: a download through
# `curl | sh` that stops half-way is a syntax error, never half an installer.
main() {
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64)               target=linux-amd64 ;;
        Linux-aarch64 | Linux-arm64) target=linux-arm64 ;;
        Darwin-arm64)               target=macos-arm64 ;;
        *)
            echo "geistr: no release for $(uname -s) $(uname -m); build it: https://github.com/geisten/geist-runtime#install" >&2
            exit 1
            ;;
    esac
    version=${GEISTR_VERSION:-latest}
    if [ -n "${GEISTR_BASE_URL:-}" ]; then # tests: a local server
        base=$GEISTR_BASE_URL
    elif [ "$version" = latest ]; then
        base=$site/latest
    else
        base=$site/$version
    fi
    if command -v curl >/dev/null; then
        fetch() { curl -fsL --retry 3 "$1" -o "$2"; }
    elif command -v wget >/dev/null; then
        fetch() { wget -q --tries=3 "$1" -O "$2"; }
    else
        echo "geistr: needs curl or wget (sudo apt install curl)" >&2
        exit 1
    fi
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM
    libs=$({ ldconfig -p || /sbin/ldconfig -p; } 2>/dev/null || true)
    case $target in linux-*)
        if printf '%s\n' "$libs" | grep -q 'libvulkan\.so\.1 ' && printf '%s\n' "$libs" | grep -q 'libgomp\.so\.1 '; then
            echo "downloading geistr-$target-vulkan ($version)"
            # a release before the Vulkan build (#253) has none: the static one then
            fetch "$base/geistr-$target-vulkan.tar.gz" "$tmp/geistr-$target-vulkan.tar.gz" && target=$target-vulkan
        fi
        ;;
    esac
    case $target in *-vulkan) ;; *) echo "downloading geistr-$target ($version)" ;; esac
    if ! { [ -f "$tmp/geistr-$target.tar.gz" ] || fetch "$base/geistr-$target.tar.gz" "$tmp/geistr-$target.tar.gz"; } ||
        ! fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS"; then
        echo "geistr: no release $version at $base (leave GEISTR_VERSION unset for the latest)" >&2
        exit 1
    fi
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
}

main
