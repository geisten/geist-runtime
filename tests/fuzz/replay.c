/* replay.c — a fuzz harness without libFuzzer (#133): each file named on the
 * command line once through LLVMFuzzerTestOneInput. make test runs the seeds
 * and every crash the fuzzers found (tests/fuzz/<harness>/) this way. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE    *f    = fopen(argv[i], "rb");
        long     n    = f && fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
        uint8_t *data = n >= 0 ? malloc((size_t) n + 1) : nullptr;
        if (!data || fseek(f, 0, SEEK_SET) != 0 || fread(data, 1, (size_t) n, f) != (size_t) n) {
            fprintf(stderr, "replay: cannot read %s\n", argv[i]);
            return 1;
        }
        fclose(f);
        LLVMFuzzerTestOneInput(data, (size_t) n);
        free(data);
    }
    printf("%s: %d inputs passed\n", argv[0], argc - 1);
    return 0;
}
