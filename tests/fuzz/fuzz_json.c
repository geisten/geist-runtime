/* fuzz_json.c — libFuzzer: the service's JSON request reader (tools/geistr/json.c
 * on jsmn), the way service.c reads a request line: parse one object, then
 * every token through the accessors (#133). */
#include "json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = malloc(size + 1); /* the service reads a NUL-terminated line */
    if (!s)
        return 0;
    memcpy(s, data, size);
    s[size]       = 0;
    struct json j = {};
    if (json_parse(&j, s, size)) {
        static const char *const keys[] = {"op", "messages", "role", "content", "temperature", "max", "stop"};
        for (int t = 0; t < j.n; t++) {
            free(json_string(&j, t));
            (void) json_number(&j, t, 0);
            (void) json_unsigned(&j, t, 0);
            uint64_t offset;
            (void) json_offset(&j, t, &offset);
            (void) json_bool(&j, t, false);
            (void) json_parent(&j, t);
            size_t      len = 0;
            const char *raw = json_raw(&j, t, &len);
            if (!raw || raw < s || raw + len > s + size)
                abort();
            for (size_t k = 0; k < sizeof keys / sizeof *keys; k++)
                (void) json_field(&j, t, keys[k]);
            for (int item = json_next(&j, t, -1); json_count(&j, t) >= 0 && item >= 0; item = json_next(&j, t, item)) {
            }
        }
    }
    json_free(&j);
    char out[64];
    json_get(s, "op", out, sizeof out);
    free(s);
    return 0;
}
