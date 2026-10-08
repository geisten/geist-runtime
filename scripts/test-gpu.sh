#!/bin/sh
# test-gpu.sh [models dir] — the GPU tests (#112): the real runtime and the CLI
# on the GPU (Vulkan on Linux, Metal on macOS) with the reference model, a
# thinking model and a GPU-sized one.
#
# The models come from the catalog into a folder of the runner's own
# (default $GEIST_CI_MODELS, else ~/geist-ci-models), SHA-256 verified and
# fetched once. The build has its own BUILD and engine checkout, so a CPU-only
# build in the same tree is not disturbed. On Linux the tests run under a
# machine-wide lock (flock on $GEIST_GPU_LOCK): GPU jobs of other repositories
# on the same runner wait instead of sharing the card.
set -eu
dir=${1:-${GEIST_CI_MODELS:-$HOME/geist-ci-models}}
for id in smollm2-360m qwen3-0.6b gemma4-e2b; do
    sh scripts/fetch-model.sh models/catalog.json "$id" "$dir"
done
file() { python3 -c 'import json, sys
print(next(m["file"] for m in json.load(open("models/catalog.json"))["models"] if m["id"] == sys.argv[1]))' "$1"; }
reference=$dir/$(file smollm2-360m)
thinking=$dir/$(file qwen3-0.6b)

case $(uname -s) in
Linux) backends="cpu_x86 cpu_scalar vulkan" ;;
*) backends="" ;; # the platform's default: Metal on macOS
esac
gpu_make() {
    make BUILD=build/gpu GEISTLIB=build/gpu-geistlib ${backends:+BACKENDS="$backends"} \
        GEIST_TEST_MODEL="$reference" "$@"
}

if command -v flock >/dev/null; then
    lock=${GEIST_GPU_LOCK:-/tmp/geist-gpu.lock}
    exec 9>"$lock"
    echo "gpu: waiting for $lock"
    flock -w 3600 9
fi
echo "gpu: $(date -u +%FT%TZ) · load $(uptime | sed 's/.*average[s]*: //')"
command -v nvidia-smi >/dev/null && nvidia-smi --query-gpu=name,memory.used,memory.total --format=csv,noheader || true

GEIST_TEST_PROCESSOR=gpu GEIST_TEST_MODEL_THINKING="$thinking" gpu_make test-real
GEISTR_TEST_GPU_MODELS="$dir" gpu_make test-geistr ONLY=gpu
