#!/bin/sh
# fetch-model.sh <catalog.json> <id> <dir> — the reference model for the real
# tests, from its catalog entry: downloaded once, SHA-256 verified.
set -eu
catalog=$1 id=$2 dir=$3
eval "$(python3 -c '
import json, sys
m = next(m for m in json.load(open(sys.argv[1]))["models"] if m["id"] == sys.argv[2])
print("file=%s url=%s sha=%s" % (m["file"], m["url"], m["sha256"]))' "$catalog" "$id")"
path=$dir/$file
sum() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
if [ -f "$path" ] && [ "$(sum "$path")" = "$sha" ]; then
    echo "reference model ready: $path"
    exit 0
fi
mkdir -p "$dir"
curl -fsSL --retry 3 --proto '=https' -o "$path.part" "$url"
if [ "$(sum "$path.part")" != "$sha" ]; then
    rm -f "$path.part"
    echo "fetch-model: $file does not match its SHA-256" >&2
    exit 1
fi
mv "$path.part" "$path"
echo "reference model ready (SHA-256 verified): $path"
