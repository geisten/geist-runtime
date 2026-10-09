/* test_fit.c's golden rankings of models/catalog.json: three computers. */
#define GOLDEN_DEVICE(k, ram_gib, gpu_) \
    {.size = sizeof(geistr_device), .kind = k, .ram = ram_gib * GIB, .available = ram_gib * GIB / 2, \
     .available_known = true, .disk = 500 * GIB, .disk_known = true, .cores = 8, .logical_cpus = 16, \
     .gpu = gpu_, .supported = true}
{"pc-8gb-cpu", GOLDEN_DEVICE(GEISTR_DEVICE_OTHER, 8, 0),
 {"gemma4-e2b", "gemma4-e4b", "bonsai2-27b-pq2", "qwen3-0.6b", "bitnet-2b", "smollm2-360m", "qwen35-0.8b", "qwen38-27b-q4", "qwen38-27b-q8", "bitnet-embed-0.6b", nullptr}},
{"mac-16gb-metal", GOLDEN_DEVICE(GEISTR_DEVICE_APPLE_SILICON, 16, GEISTR_BACKEND_METAL),
 {"gemma4-e2b", "gemma4-e4b", "bonsai2-27b-pq2", "qwen38-27b-q4", "qwen3-0.6b", "bitnet-2b", "smollm2-360m", "qwen35-0.8b", "qwen38-27b-q8", "bitnet-embed-0.6b", nullptr}},
{"mac-64gb-metal", GOLDEN_DEVICE(GEISTR_DEVICE_APPLE_SILICON, 64, GEISTR_BACKEND_METAL),
 {"gemma4-e2b", "gemma4-e4b", "bonsai2-27b-pq2", "qwen38-27b-q4", "qwen38-27b-q8", "qwen3-0.6b", "bitnet-2b", "smollm2-360m", "qwen35-0.8b", "bitnet-embed-0.6b", nullptr}},
