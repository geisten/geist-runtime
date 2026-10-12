#!/bin/sh
# check-version.sh — every place that names geistr's version names the current one (#148):
# geistr_version() in src/runtime.c is the source; src/stub.c, include/geistr.h and
# python/geistr/__init__.py carry it too, and in README.md and install.sh every vX.Y[.Z]
# must be vVERSION (a release PR changes all of them; older versions there without the v).
set -eu
cd "$(dirname "$0")/.."
v=$(sed -n 's/.*return "\([0-9.]*\)";.*/\1/p' src/runtime.c | head -1)
[ -n "$v" ] || { echo "check-version: no version in src/runtime.c" >&2; exit 1; }
bad=0
grep -q "return \"$v-stub\";" src/stub.c || { echo "check-version: src/stub.c is not $v-stub" >&2; bad=1; }
grep -q "^/\* \"$v\" — " include/geistr.h || { echo "check-version: include/geistr.h does not name $v" >&2; bad=1; }
grep -q "^__version__ = \"$v\"\$" python/geistr/__init__.py || { echo "check-version: python/geistr/__init__.py is not $v" >&2; bad=1; }
for f in README.md install.sh; do
    other=$(grep -noE '(^|[^0-9A-Za-z.])v[0-9]+\.[0-9]+(\.[0-9]+)?' "$f" | grep -vE "[^0-9A-Za-z.]v$v\$|:v$v\$" || true)
    [ -z "$other" ] || { echo "check-version: $f names another version than v$v (line:text): $other" >&2; bad=1; }
done
[ $bad -eq 0 ] && echo "version $v everywhere"
exit $bad
