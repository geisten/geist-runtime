/* fuzz_catalog.c — libFuzzer: geistr_catalog_parse (#133). */
#include "geistr_catalog.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    geistr_catalog *c = nullptr;
    char            error[256];
    if (geistr_catalog_parse((const char *) data, size, &c, error, sizeof error) == GEISTR_OK) {
        for (size_t i = 0; i < geistr_catalog_count(c); i++) /* every entry is there and terminated */
            if (geistr_catalog_find(c, geistr_catalog_get(c, i)->id) == nullptr)
                abort();
        geistr_catalog_free(c);
    } else if (c || strnlen(error, sizeof error) == sizeof error)
        abort();
    return 0;
}
