#!/bin/sh
# apt-need.sh COMMAND[:PACKAGE]… — install the packages whose command is missing.
# The CI runners' images usually have them; apt mirrors sometimes hang, so
# apt runs only when needed, with network timeouts, three times at most.
need=
for item in "$@"; do
    command -v "${item%%:*}" >/dev/null 2>&1 || need="$need ${item#*:}"
done
[ -z "$need" ] && { echo "all present: $*"; exit 0; }
echo "installing:$need"
for try in 1 2 3; do
    sudo timeout 180 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 update -q &&
        sudo timeout 300 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 install -y -q $need && exit 0
    echo "apt failed (try $try), again in 10 s" >&2
    sleep 10
done
exit 1
