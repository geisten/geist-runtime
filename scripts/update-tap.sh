#!/bin/sh
# update-tap.sh TAG SUMS TAPDIR — write TAPDIR/Formula/geistr.rb for release TAG
# from its SHA256SUMS (archives at www.geisten.net/download/geistr/TAG/).
set -eu
tag=$1 sums=$2 tap=$3
version=${tag#v}
base=https://www.geisten.net/download/geistr/$tag
sum() { awk -v f="geistr-$1.tar.gz" '$2 == f { print $1 }' "$sums" | grep . || { echo "update-tap: no $1 in $sums" >&2; exit 1; }; }
mkdir -p "$tap/Formula"
cat > "$tap/Formula/geistr.rb" <<RB
class Geistr < Formula
  desc "Local LLM chat, model catalog and service on the geist engine (GGUF)"
  homepage "https://github.com/geisten/geist-runtime"
  version "$version"
  license "Apache-2.0"

  on_macos do
    on_arm do
      url "$base/geistr-macos-arm64.tar.gz"
      sha256 "$(sum macos-arm64)"
    end
  end
  on_linux do
    on_arm do
      url "$base/geistr-linux-arm64.tar.gz"
      sha256 "$(sum linux-arm64)"
    end
    on_intel do
      url "$base/geistr-linux-amd64.tar.gz"
      sha256 "$(sum linux-amd64)"
    end
  end

  def install
    bin.install "geistr"
  end

  def caveats
    <<~EOS
      Install a model and chat:
        geistr catalog
        geistr pull smollm2-360m
        geistr chat smollm2-360m
      The OpenAI and Ollama APIs (replacing geist-serve):
        geistr serve smollm2-360m --http
    EOS
  end

  test do
    assert_match "geistr #{version}", shell_output("#{bin}/geistr --version")
  end
end
RB
