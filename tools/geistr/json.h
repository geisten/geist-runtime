/* json.h — the JSON geistr's tools read and write: one object per line (the
 * service protocol, chat files, the catalog's speed references), parsed with
 * jsmn. Tokens are indexes; the object is token 0. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct json {
    const char *s;
    void       *tokens; /* jsmntok_t[] */
    int         n;
};

bool   json_parse(struct json *j, const char *s, size_t len); /* an object; json_free after, also on failure */
void   json_free(struct json *j);
int    json_field(const struct json *j, int object, const char *key); /* its value's token, or -1 */
int    json_count(const struct json *j, int t);                       /* an array's elements, else -1 */
int    json_next(const struct json *j, int array, int item); /* the element after item (-1: the first), or -1 */
char  *json_string(const struct json *j, int t);        /* unescaped, malloc'd; nullptr if no string */
/* t's JSON text as it stands in the input (an object or array with its
 * brackets, a string without its quotes); nullptr if t is no token. */
const char *json_raw(const struct json *j, int t, size_t *len);
double json_number(const struct json *j, int t, double dflt);
bool   json_bool(const struct json *j, int t, bool dflt); /* true or false, else dflt */

/* A top-level string or number of one object as text into out ("" if absent). */
void json_get(const char *object, const char *key, char *out, size_t cap);

/* s as a JSON string, quoted and escaped. */
void json_write(FILE *f, const char *s);
