#pragma once

#include "FrameCrypto.h"
#include "MacWire.h"

#include <cstdint>
#include <optional>
#include <vector>

// The sim's MAC wire v1 crypto (docs/mac-wire-format.md), on host mbedtls; the
// firmware uses PSA against the same library, so frames are byte-identical.

namespace proto_sim {

// The sim's K_root: SHA-256 of a test-only phrase, truncated to 16 bytes
// (crypto.cpp kKeyString; kept across calls, idempotent).
const uint8_t* root_key();

// The on-air tag length: CMAC-8.
constexpr size_t kOnAirTagBytes = 8;

// ---------------------------------------------------------------------------
// MAC wire v1 (S8, docs/mac-wire-format.md section 4): the sim's own
// implementation of the raw-frame tags, on mbedtls (the hub uses PSA). The tag
// is CMAC-8 over "BW1" || dir || id u32 || frame[0 .. L-8). The header itself
// is encoded with the shared MacWire.h (pinned by the node's vectors).
// ---------------------------------------------------------------------------

// Raw-key primitives (the shared vectors use fixed keys).
void v1_tag(const uint8_t key[16], bool downlink, uint32_t id, const uint8_t *bytes, size_t n,
            uint8_t out[8]);
void v1_ctr(const uint8_t key[16], uint32_t session_id, uint8_t node_addr, bool downlink,
            uint32_t fcnt, const uint8_t *in, size_t n, uint8_t *out);
// The K_enc / K_mac a hub and node derive for (session, node nonce, hub, node).
void v1_session_keys(uint32_t session_id, uint32_t node_nonce, uint8_t hub_addr, uint8_t node_addr,
                     uint8_t k_enc[16], uint8_t k_mac[16]);

// Whole frames. `h.flags` must already carry the AUTH the helper implements.
std::vector<uint8_t> v1_session_frame(uint32_t session_id, uint32_t node_nonce, uint8_t hub_addr,
                                      uint8_t node_addr, bool downlink, const macwire::WireHdr &h,
                                      const std::vector<uint8_t> &body);
std::vector<uint8_t> v1_kauth_frame(bool downlink, const macwire::WireHdr &h,
                                    const std::vector<uint8_t> &body);
std::vector<uint8_t> v1_plain_frame(const macwire::WireHdr &h, const std::vector<uint8_t> &body);
// Tag check THEN CTR. nullopt on a bad shape or tag. *hdr_out (optional) is
// the decoded header.
std::optional<std::vector<uint8_t>> v1_open_session_frame(uint32_t session_id, uint32_t node_nonce,
                                                          uint8_t hub_addr, uint8_t node_addr,
                                                          bool downlink,
                                                          const std::vector<uint8_t> &frame,
                                                          macwire::WireHdr *hdr_out = nullptr);
bool v1_verify_kauth_frame(bool downlink, const std::vector<uint8_t> &frame);

} // namespace proto_sim
