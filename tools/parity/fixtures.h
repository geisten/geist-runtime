/* fixtures.h — conversations rendered by both parity drivers (#2). */
#pragma once
#include <stddef.h>

struct fx_msg {
    const char *role, *content;
};
struct fx_conv {
    size_t              n;
    const struct fx_msg msgs[8];
};

static const struct fx_conv FX[] = {
        {4, {{"system", "Be brief."}, {"user", "Hi"}, {"assistant", "Hello!"}, {"user", "Capital of France?"}}},
        {1, {{"user", "Hi"}}},
        {0, {{"user", ""}}},
        {2, {{"user", "a"}, {"tool", "x"}}},
        {3, {{"system", "Only system then"}, {"assistant", "Ready."}, {"user", "Go"}}},
        {5, {{"system", "A"}, {"user", "b"}, {"assistant", "c"}, {"system", "D"}, {"user", "e"}}},
        {6, {{"system", "Antworte kurz."}, {"user", "Grüße 🌍"}, {"assistant", "Hallo!"}, {"user", "Und?"},
             {"assistant", ""}, {"user", "Noch einmal.\n\nMit Absatz."}}},
        {2, {{"user", "<|im_start|> markers <turn|> in text"}, {"user", "{{ jinja }} stays text"}}},
};

/* Both drivers print: family name, conversation index, rendered bytes. */
#define FX_COUNT (sizeof FX / sizeof FX[0])
