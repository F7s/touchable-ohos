#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/app"
devecocli build --build-mode debug --assemble-type assembleHap
echo
echo "HAP: $(pwd)/entry/build/default/outputs/default/entry-default-signed.hap"
