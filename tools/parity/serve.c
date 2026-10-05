/* serve.c — geist-serve's renderer over the fixtures (compiled against SERVE_DIR/src). */
#include "template.h"
#include "fixtures.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    for (enum chat_family f = CHAT_GEMMA3; f <= CHAT_BITNET; f++)
        for (size_t c = 0; c < FX_COUNT; c++) {
            struct chat_msg msgs[8];
            for (size_t i = 0; i < FX[c].n; i++)
                msgs[i] = (struct chat_msg) {FX[c].msgs[i].role, FX[c].msgs[i].content};
            char *p = chat_render(f, FX[c].n, msgs);
            printf("== %s %zu\n%s\n", chat_family_name(f), c, p ? p : "(null)");
            free(p);
        }
    return 0;
}
