/* files.h — your own files in the chat (#91): geistr index DIR, chat --files DIR. */
#pragma once
#include <stddef.h>

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
