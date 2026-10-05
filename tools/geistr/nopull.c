/* nopull.c — geistr without the download module: no network code. */
#include "pull.h"
#include <stdio.h>

int geistr_pull(const geistr_catalog_entry *entry, const char *models_dir) {
    fprintf(stderr, "geistr: this build has no download module; get %s from\n  %s\ninto %s\n", entry->file,
            entry->url, models_dir);
    return 1;
}
