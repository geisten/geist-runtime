/* geistr.c — the runtime's renderer over the same fixtures, one send each. */
#include "template.h"
#include "fixtures.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    for (enum tpl_family f = TPL_GEMMA3; f <= TPL_BITNET; f++)
        for (size_t c = 0; c < FX_COUNT; c++) {
            geistr_message msgs[8];
            for (size_t i = 0; i < FX[c].n; i++)
                msgs[i] = (geistr_message) {FX[c].msgs[i].role, FX[c].msgs[i].content};
            struct tpl_state st;
            tpl_init(&st, f);
            char *p = tpl_render_send(&st, FX[c].n, msgs);
            tpl_free(&st);
            printf("== %s %zu\n%s\n", tpl_family_name(f), c, p ? p : "(null)");
            free(p);
        }
    return 0;
}
