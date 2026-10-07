/* cli.h — what the parts of the geistr CLI share. */
#pragma once
#include <stdbool.h>

enum { OK = 0, ERROR = 1, USAGE = 2, CANCELLED = 130 }; /* exit codes */

/* ---- config.c: settings in geistr.conf, the data folder ------------------ */

struct settings {
    char   model[256], processor[8], system[2048];
    double temperature;
    bool   markdown, stats, intro, resume;
};
extern struct settings cfg;
extern char            config_path[4200], data_dir[4096]; /* "" when unknown */

bool data_folder(void); /* find both; false without HOME or GEISTEN_HOME */
void config_load(void);
bool config_save(void);
int  config(int n, const char **args); /* geistr config [key [value]] */

bool make_dirs(const char *path, unsigned mode); /* mkdir -p */
