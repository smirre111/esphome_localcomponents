#pragma once
// v1_bridge.h -- test-side translation between the v0 envelope and MAC wire v1.
//
// S8 / W4: the hub's default wire is v1 (MAC_WIRE_V1). Hundreds of listener
// tests were written against the v0 envelope: they BUILD node uplinks as v0
// LoraClientResponseMessage frames (sealed with the v0 Encrypt-then-CMAC
// prefix) and INSPECT hub downlinks as v0 LoraClientOperationMessage frames.
// What they test is behaviour (sessions, ARQ, timing, scheduling), not the v0
// bytes, so instead of rewriting each one the harness translates at the two
// boundaries:
//   * uplinkToV1: a v0 uplink a test built -> the v1 frame a real node would
//     send (same header values, same body message, same keys). A frame whose
//     v0 tag / MIC does NOT verify becomes a v1 frame whose tag does not either,
//     so the refusal tests keep their meaning.
//   * downlinkToV0: a hub v1 frame -> the v0 envelope the test inspects,
//     re-sealed with the v0 construction under the same keys so a test that
//     opens it with the v0 sim helpers still can.
// Keys: the session id is the hub's (a lookup the harness installs); the node
// nonce is the one the test used (seen in a SESSION_OPENING / sessionNonce,
// else the suite-wide default kDefaultNodeNonce).
//
// Tests that pin v0 BYTES (non-canonical lengths, spliced headers, v0 golden
// files, the v0 CMAC prefix) are not translatable by design and are retired in
// ON with a reason (mac_wire_v0_only_tests.txt).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace proto_sim {
namespace v1bridge {

constexpr uint32_t kDefaultNodeNonce = 0xACE55001u;   // every suite's kTestNodeNonce

// The hub's session id for a node (installed by the harness that links the hub).
using SessionLookup = bool (*)(uint8_t node, uint32_t *session_id);
void setSessionLookup(SessionLookup f);

void     noteNodeNonce(uint8_t node, uint32_t nonce);
uint32_t nodeNonce(uint8_t node);
void     reset();

// Identity when the input is not a frame of the source format.
std::vector<uint8_t> uplinkToV1(const uint8_t *data, size_t len);
std::vector<uint8_t> downlinkToV0(const uint8_t *data, size_t len);

}  // namespace v1bridge
}  // namespace proto_sim
