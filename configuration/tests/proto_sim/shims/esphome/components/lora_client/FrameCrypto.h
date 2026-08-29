// Re-export of the vendored production header at the path the .cpp expects.
// Angle-bracket include skips the current-dir search and resolves via
// -I.../local_components/lora_client (set in CMakeLists), which is where the
// hub's vendored copy of FrameCrypto.h lives.
//
// The harness deliberately compiles the HUB's copy here, not the node's — that
// way the ctest gate `wire_format_drift_hub_vs_node` is what proves they agree,
// rather than the harness quietly papering over a divergence.
#pragma once
#include <FrameCrypto.h>
