// Offline independent oracle, built against a revision-pinned Prism/llama.cpp.
// No weights/GPU are loaded; each stdin line is a hex-encoded native prompt.
#include "llama.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    auto params = llama_model_default_params();
    params.vocab_only = true;
    params.n_gpu_layers = 0;
    llama_model *model = llama_model_load_from_file(argv[1], params);
    if (!model)
        return 1;
    const llama_vocab *vocab = llama_model_get_vocab(model);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line.size() % 2 || line.size() > 2 * 262144) {
            llama_model_free(model);
            return 2;
        }
        std::string text;
        text.reserve(line.size() / 2);
        auto nibble = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        for (std::size_t i = 0; i < line.size(); i += 2) {
            int a = nibble(line[i]), b = nibble(line[i + 1]);
            if (a < 0 || b < 0) {
                llama_model_free(model);
                return 2;
            }
            text.push_back(static_cast<char>((a << 4) | b));
        }
        int32_t count =
            llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, false, true);
        if (count >= 0 || count == INT32_MIN) {
            llama_model_free(model);
            return 1;
        }
        std::vector<llama_token> ids(static_cast<std::size_t>(-count));
        count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), ids.data(),
                               static_cast<int32_t>(ids.size()), false, true);
        if (count < 0) {
            llama_model_free(model);
            return 1;
        }
        std::cout << '[';
        for (int32_t i = 0; i < count; ++i)
            std::cout << (i ? "," : "") << ids[static_cast<std::size_t>(i)];
        std::cout << "]\n";
    }
    llama_model_free(model);
    return 0;
}
