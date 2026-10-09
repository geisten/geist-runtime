"""Check every download in models/catalog.json against Hugging Face, without
downloading: the URL answers, and its size and SHA-256 are the catalog's.

  check_catalog.py [--catalog models/catalog.json] [--pin]

A vision tower's bytes and sha256 describe what geistr extracts from its
download, so only its answer is checked. A HEAD request per file reads X-Linked-Size and X-Linked-ETag (the SHA-256 of
the stored file) and X-Repo-Commit. --pin rewrites a `resolve/main` URL to
`resolve/<commit>` when that commit serves exactly the catalog's file, so a
later push to the model repository cannot change what geistr downloads.
Exit 1 if any file is missing or differs. Standard library only."""
import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


def head(url):
    """(status, size, sha256, commit) of a Hugging Face resolve URL."""
    opener = urllib.request.build_opener(NoRedirect)
    request = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'geistr-catalog-check'})
    try:
        r = opener.open(request, timeout=60)
    except urllib.error.HTTPError as e:  # the 302 to the CDN carries the headers
        r = e
    h = r.headers
    size = h.get('X-Linked-Size') or h.get('Content-Length')
    sha = (h.get('X-Linked-ETag') or h.get('ETag') or '').strip('"').removeprefix('W/').strip('"')
    return r.status, int(size) if size else None, sha, h.get('X-Repo-Commit')


def files(catalog):
    """(label, entry, key path, url, bytes, sha256) of every download."""
    for m in catalog['models']:
        yield m['id'], m, ('url',), m['url'], m['bytes'], m['sha256']
        if 'vision' in m:
            v = m['vision']
            # bytes and sha256 describe the vision tensors geistr extracts from this
            # download, not the download: only that it answers is checked here
            yield m['id'] + ' vision', v, ('url',), v['url'], None, None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--catalog', type=Path, default=ROOT / 'models/catalog.json')
    p.add_argument('--pin', action='store_true', help='rewrite resolve/main URLs to the serving commit')
    a = p.parse_args()
    catalog = json.loads(a.catalog.read_text())
    failures, pinned = 0, 0
    for label, entry, _, url, size, sha in files(catalog):
        status, got_size, got_sha, commit = head(url)
        ok = status in (200, 302) and (size is None or got_size == size) and (sha is None or got_sha == sha)
        unpinned = '/resolve/main/' in url
        print(f'{"✓" if ok else "✗"} {label:24} {status} size {got_size} sha {got_sha[:12]}…'
              f'{"  (resolve/main)" if unpinned else ""}')
        if not ok:
            failures += 1
            print(f'    catalog: size {size} sha {sha}', file=sys.stderr)
        elif a.pin and unpinned and commit and re.fullmatch(r'[0-9a-f]{40}', commit):
            entry['url'] = url.replace('/resolve/main/', f'/resolve/{commit}/')
            pinned += 1
    if pinned:
        a.catalog.write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + '\n')
        print(f'pinned {pinned} URL(s) to their commit')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
