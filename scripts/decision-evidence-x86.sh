#!/bin/sh
# decision-evidence-x86.sh [out dir] — #94's x86 CPU decision evidence on the
# current engine pin, run by the decision-evidence workflow of
# git.geisten.net/geisten-hw/geist-runtime-hw on the self-hosted x86 runner: Gemma 4 E2B's wrapper/direct-engine records, the CLI and the
# Python controls (DECISION=1, cpu_x86), with the host, the engine and the
# binaries' hashes. Under the runner's machine lock, like the GPU tests.
set -eu
out=${1:-build/evidence-x86}
dir=${GEIST_CI_MODELS:-$HOME/geist-ci-models}
mkdir -p "$out"
sh scripts/fetch-model.sh models/catalog.json gemma4-e2b "$dir"
model=$dir/$(python3 -c 'import json
print(next(m["file"] for m in json.load(open("models/catalog.json"))["models"] if m["id"] == "gemma4-e2b"))')

if command -v flock >/dev/null; then
    exec 9>"${GEIST_GPU_LOCK:-/tmp/geist-gpu.lock}"
    flock -w 3600 9
fi
dec_make() {
    make BUILD=build/dec-x86 GEISTLIB=build/dec-x86-geistlib BACKENDS="cpu_x86 cpu_scalar" DECISION=1 \
        DECISION_MODEL="$model" DECISION_PROFILE=gemma4 DECISION_PROCESSOR=cpu "$@"
}
dec_make build/dec-x86/test_decision_real build/dec-x86/geistr
{
    echo "date $(date -u +%FT%TZ)"
    echo "engine $(sed -n 's/^GEIST_REF  ?= //p' Makefile)"
    echo "source $(git rev-parse HEAD)"
    echo "uname $(uname -srm)"
    lscpu 2>/dev/null | grep -E '^(Model name|CPU\(s\)|Thread|Flags)' | cut -c1-200 || true
    free -g 2>/dev/null | sed -n 2p || true
    echo "load $(uptime | sed 's/.*average[s]*: //')"
    sha256sum build/dec-x86/test_decision_real build/dec-x86/geistr "$model"
} > "$out/host.txt"
build/dec-x86/test_decision_real "$model" gemma4 cpu > "$out/records-gemma4-cpu-x86.jsonl"
dec_make -s test-decision-cli > "$out/cli-gemma4-cpu-x86.jsonl"
dec_make -s test-decision-python > "$out/python-gemma4-cpu-x86.jsonl"
echo "decision evidence: $(ls "$out")"
