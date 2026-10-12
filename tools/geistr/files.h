/* files.h — your own files in the chat (#91): geistr index DIR, chat --files DIR. */
#pragma once
#include <stddef.h>

/* Put before a question and before a passage when they are embedded. Nothing
 * before either found the right file best with bitnet-embed-0.6b (#157,
 * scripts/retrieval-bench.py: top-4 97 %, top-1 86 %; the former Instruct
 * prefix 92 % and 64 %). The passage prefix is in every index's vectors:
 * changing it needs a new INDEX_VERSION in files.c, so that old indexes are
 * made anew instead of mixing two kinds of vectors. Pinned by test_embed.c. */
#define FILES_QUERY_PREFIX   ""
#define FILES_PASSAGE_PREFIX ""

struct files;
/* The folder's index, brought up to date (new and changed files embedded,
 * removed ones dropped) with the catalog's embedding model; quiet: no
 * progress and no summary. nullptr after saying why. */
struct files *files_open(const char *dir, bool quiet);
void          files_close(struct files *f);
const char   *files_dir(const struct files *f);
/* The question with the nearest excerpts before it (malloc'd); their files
 * into sources ("a.md, notes/b.txt"). nullptr: the embedding failed. */
char *files_ask(struct files *f, const char *question, char *sources, size_t cap);
