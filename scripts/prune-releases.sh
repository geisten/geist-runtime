#!/bin/sh
# prune-releases.sh KEEP TAG DEST — the download server keeps the newest KEEP releases (#148).
# DEST: the rsync destination of the geistr folder (in .gitea/workflows/release.yml
# germar@www.geisten.net:geistr/, ssh through RSYNC_RSH). The key there is write-only
# (rrsync), so the folder is listed by a dry run that would delete all of it.
# Deleted: the directories vX.Y.Z older than the newest KEEP (in version order, v0.10.0
# after v0.9.9). Never `latest`, `install.sh`, any other name, or TAG (the release just
# uploaded). Nothing is deleted when TAG is not on the server. DRY_RUN=1 only prints.
set -eu
[ $# -eq 3 ] || { echo "usage: prune-releases.sh KEEP TAG DEST" >&2; exit 2; }
keep=$1 tag=$2 dest=$3
case $keep in '' | *[!0-9]* | 0) echo "prune-releases: KEEP must be a number >= 1, not '$keep'" >&2; exit 2 ;; esac
empty=$(mktemp -d)
trap 'rmdir "$empty"' EXIT
names=$(rsync -rn --delete -v "$empty/" "$dest" | sed -n 's|^deleting \([^/]*\)/\{0,1\}$|\1|p')
releases=$(printf '%s\n' "$names" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -t. -k1.2,1nr -k2,2nr -k3,3nr || true)
echo "on the server: $(echo $releases)"
printf '%s\n' "$releases" | grep -qxF "$tag" || { echo "prune-releases: $tag is not at $dest; nothing deleted" >&2; exit 1; }
old=$(printf '%s\n' "$releases" | tail -n +$((keep + 1)) | grep -vxF "$tag" || true)
echo "kept: the newest $keep and $tag; deleted: $(echo ${old:-nothing})${DRY_RUN:+ (DRY_RUN, nothing changes)}"
[ -n "$old" ] || exit 0
set --
for d in $old; do set -- "$@" --include="/$d/"; done
[ -n "${DRY_RUN:-}" ] && set -- -n "$@"
rsync -r --delete -v "$@" --exclude='/*' "$empty/" "$dest"
