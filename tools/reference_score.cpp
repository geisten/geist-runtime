// Independent development oracle on the exact GGUF: the latest upstream llama.cpp
// release (tools/llama_oracle.py builds and runs it).
// stdin: n_prompt n_candidates prompt_ids... candidate_ids... (one case/line).
// CPU, six threads, F32 KV, no flash attention; last prompt position only.
#include "llama.h"
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    ggml_backend_load_all(); /* a build with dynamic backends; a no-op for static ones */
    auto mp = llama_model_default_params();
    ggml_backend_dev_t cpu_only[] = {nullptr};
    mp.devices = cpu_only;
    mp.n_gpu_layers = 0;
    llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model)
        return 1;
    auto cp = llama_context_default_params();
    cp.n_ctx = 512;
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_threads = 6;
    cp.n_threads_batch = 6;
    cp.type_k = GGML_TYPE_F32;
    cp.type_v = GGML_TYPE_F32;
    cp.offload_kqv = false;
    cp.op_offload = false;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    llama_context *context = llama_init_from_model(model, cp);
    if (!context) {
        llama_model_free(model);
        return 1;
    }
    const int32_t vocab = llama_vocab_n_tokens(llama_model_get_vocab(model));
    std::string line;
    int code = 0;
    while (std::getline(std::cin, line)) {
        if (line.size() > 16384) {
            code = 2;
            break;
        }
        std::istringstream input(line);
        int n = 0, k = 0;
        if (!(input >> n >> k) || n < 1 || n > 512 || k < 1 || k > 26) {
            code = 2;
            break;
        }
        std::vector<llama_token> prompt(static_cast<size_t>(n)), candidates(static_cast<size_t>(k));
        bool valid = true;
        for (auto &id : prompt)
            valid = valid && static_cast<bool>(input >> id) && id >= 0 && id < vocab;
        for (auto &id : candidates)
            valid = valid && static_cast<bool>(input >> id) && id >= 0 && id < vocab;
        std::string extra;
        if (!valid || input >> extra) {
            code = 2;
            break;
        }
        llama_memory_clear(llama_get_memory(context), true);
        auto batch = llama_batch_init(n, 0, 1);
        batch.n_tokens = n;
        for (int i = 0; i < n; ++i) {
            batch.token[i] = prompt[static_cast<size_t>(i)];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = i == n - 1;
        }
        const int decoded = llama_decode(context, batch);
        llama_batch_free(batch);
        const float *logits = decoded == 0 ? llama_get_logits_ith(context, -1) : nullptr;
        if (!logits) {
            code = 1;
            break;
        }
        size_t best = 0;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!std::isfinite(logits[candidates[i]])) {
                valid = false;
                break;
            }
            if (logits[candidates[i]] > logits[candidates[best]])
                best = i;
        }
        if (!valid) {
            code = 1;
            break;
        }
        std::cout << "{\"best_index\":" << best << ",\"logits\":[" << std::setprecision(9);
        for (size_t i = 0; i < candidates.size(); ++i)
            std::cout << (i ? "," : "") << logits[candidates[i]];
        std::cout << "]}\n";
    }
    llama_free(context);
    llama_model_free(model);
    return code;
}
