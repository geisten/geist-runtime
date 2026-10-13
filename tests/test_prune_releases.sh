#!/bin/sh
# scripts/prune-releases.sh against a local folder shaped like www.geisten.net/download/geistr/ (#148).
set -eu
prune="sh $(cd "$(dirname "$0")/.." && pwd)/scripts/prune-releases.sh"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
fail() { echo "test_prune_releases: $*" >&2; exit 1; }
server() { # a fresh folder with these release directories, latest, install.sh and a stranger
    rm -rf "$t/geistr" && mkdir -p "$t/geistr"
    for v in "$@"; do mkdir "$t/geistr/$v" && echo x > "$t/geistr/$v/SHA256SUMS"; done
    ln -s "$1" "$t/geistr/latest"
    echo '#!/bin/sh' > "$t/geistr/install.sh"
    mkdir "$t/geistr/notes" "$t/geistr/v1.0.0-rc1"
}
left() { (cd "$t/geistr" && ls | tr '\n' ' '); }
expect() { [ "$(left)" = "$1" ] || fail "$2: left '$(left)', expected '$1'"; }

# semver order, not lexical: v0.10.0 is the newest, v0.9.x older
server v0.10.0 v0.3.2 v0.3.3 v0.3.10 v0.9.1 v0.3.4 v0.9.0
$prune 5 v0.10.0 "$t/geistr/" > /dev/null
expect "install.sh latest notes v0.10.0 v0.3.10 v0.3.4 v0.9.0 v0.9.1 v1.0.0-rc1 " "keep 5"

# the tag just uploaded stays, even when it is not among the newest (a fix for an older line)
server v0.3.5 v0.4.0 v0.4.1 v0.4.2
$prune 2 v0.3.5 "$t/geistr/" > /dev/null
expect "install.sh latest notes v0.3.5 v0.4.1 v0.4.2 v1.0.0-rc1 " "the tag stays"

# DRY_RUN: prints, deletes nothing
server v0.3.2 v0.3.3 v0.3.4
out=$(DRY_RUN=1 $prune 1 v0.3.4 "$t/geistr/")
expect "install.sh latest notes v0.3.2 v0.3.3 v0.3.4 v1.0.0-rc1 " "DRY_RUN"
echo "$out" | grep -q 'deleted: v0.3.3 v0.3.2 (DRY_RUN' || fail "DRY_RUN plan: $out"

# fewer than KEEP: nothing to delete
server v0.3.2 v0.3.3
$prune 5 v0.3.3 "$t/geistr/" > /dev/null
expect "install.sh latest notes v0.3.2 v0.3.3 v1.0.0-rc1 " "fewer than KEEP"

# the tag is not on the server (upload went elsewhere): refuses, deletes nothing
server v0.3.2 v0.3.3 v0.3.4
if $prune 1 v0.3.9 "$t/geistr/" > /dev/null 2>&1; then fail "a missing tag must fail"; fi
expect "install.sh latest notes v0.3.2 v0.3.3 v0.3.4 v1.0.0-rc1 " "missing tag"

# KEEP must be a positive number
for k in 0 x ''; do
    if $prune "$k" v0.3.4 "$t/geistr/" > /dev/null 2>&1; then fail "KEEP '$k' must fail"; fi
done
expect "install.sh latest notes v0.3.2 v0.3.3 v0.3.4 v1.0.0-rc1 " "bad KEEP"
echo "prune-releases: 6 cases passed"
