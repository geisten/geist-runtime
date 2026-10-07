/* pull.h — the optional download module of geistr (#11). A build without
 * it (nopull.c) has no network code at all. */
#pragma once
#include "geistr_catalog.h"

/* Download entry into models_dir, resuming a .part file, verify its SHA-256
 * and move it into place. Returns a geistr exit code (0, 1, 130). */
int geistr_pull(const geistr_catalog_entry *entry, const char *models_dir);

/* mkdir -p with mode for each missing part (geistr.c). */
bool make_dirs(const char *path, unsigned mode);
