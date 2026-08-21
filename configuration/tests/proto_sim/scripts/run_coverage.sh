#!/usr/bin/env bash
# Build the test suite with coverage instrumentation, run every scenario,
# generate gcovr reports, and enforce per-file thresholds.
#
# Usage:  bash tests/proto_sim/scripts/run_coverage.sh [build_dir]
set -euo pipefail

BUILD_DIR="${1:-build/proto_sim_cov}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CFG_DIR="$(cd "$SRC_DIR/../.." && pwd)"

cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DPROTO_SIM_COVERAGE=ON
cmake --build "$BUILD_DIR" -j
ctest --test-dir "$BUILD_DIR" --output-on-failure

mkdir -p "$BUILD_DIR/coverage_reports"

# Two reports:
#   1. Production hub-side code (lora_client.cpp + lora_cover.cpp +
#      lora_sensor.cpp + lora_tracker.cpp + blinds.pb-c.c). These are the
#      files that ship to devices and the harness's primary purpose is to
#      exercise them.
#   2. Protocol model + crypto helpers (sim/*.cpp). Lower bar since these
#      are test scaffolding, but should still be well-covered.

# Coverage thresholds — set to today's baseline (the current real-code
# scenarios exercise only a handful of paths in the production source).
# Raise these numbers as more scenarios get ported off the model and on
# to the real code:
#   * Today (Jun 2026): lora_client.cpp ~30%, CmdDispatcher.cpp ~24%.
#   * Phase-3 roadmap goal: ≥ 75% on both once the model scenarios
#     (A4, B3, C1–C3, D1–D5, E1–E6) are mirrored against real code.
PROD_THRESHOLD="${PROD_THRESHOLD:-20}"
MODEL_THRESHOLD="${MODEL_THRESHOLD:-60}"

gcovr --root "$CFG_DIR" \
      --filter "local_components/lora_client/.*" \
      --filter "local_components/lora_tracker/.*" \
      --filter "local_components/loracover/.*" \
      --filter "local_components/blindsproto/.*" \
      --filter ".*/BlindsESP/main/CmdDispatcher.cpp" \
      --filter ".*/BlindsESP/main/comm_utils.c" \
      --txt "$BUILD_DIR/coverage_reports/production.txt" \
      --html-details "$BUILD_DIR/coverage_reports/production.html" \
      --fail-under-line "$PROD_THRESHOLD" \
      "$BUILD_DIR"

gcovr --root "$CFG_DIR" \
      --filter "tests/proto_sim/sim/.*" \
      --exclude "tests/proto_sim/sim/messages.h" \
      --txt "$BUILD_DIR/coverage_reports/model.txt" \
      --fail-under-line "$MODEL_THRESHOLD" \
      "$BUILD_DIR"

echo
echo "=== Production coverage ==="
cat "$BUILD_DIR/coverage_reports/production.txt"
echo
echo "=== Model coverage ==="
cat "$BUILD_DIR/coverage_reports/model.txt"
