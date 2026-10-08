/* nopull.c — geistr without the download module: no network code. */
#include "pull.h"
#include <stdio.h>

int geistr_pull(const geistr_catalog_entry *entry, const char *models_dir) {
    fprintf(stderr, "geistr: this build has no download module; get %s from\n  %s\ninto %s\n", entry->file,
            entry->url, models_dir);
    return 1;
}

int geistr_pull_vision(const geistr_catalog_entry *entry, const char *models_dir) {
    (void) models_dir;
    if (entry->vision_url)
        fputs("geistr: this geistr has no download module (built with PULL=0)\n", stderr);
    return entry->vision_url ? 1 : 0;
}
