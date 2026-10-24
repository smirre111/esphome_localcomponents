#pragma once

#include "sim/messages.h"

#include <cstdint>
#include <optional>
#include <vector>

// The sim codec: the proto_sim plain structs (messages.h) to and from MAC wire
// v1 frames (fixed MacWire.h header + one body PDU), using the generated stubs
// the production firmware compiles. See wire_codec.cpp's banner for the
// mapping. A frame production must ACCEPT as sealed is built with seal_resp and
// a sealed hub downlink is read with open_op (the sim's own v1 crypto).

namespace proto_sim {

std::vector<uint8_t> serialize_op(const LoraClientOperationMessage& m);
std::vector<uint8_t> serialize_resp(const LoraClientResponseMessage& m);

// The body PDU alone (no header): what goes under the CTR seal.
std::vector<uint8_t> serialize_op_payload(const LoraClientOperationMessage& m);
std::vector<uint8_t> serialize_resp_payload(const LoraClientResponseMessage& m);

// Returns std::nullopt on parse failure (the production "Could not read
// protobuf" path). Tests for H2/H3 rely on this.
std::optional<LoraClientOperationMessage> deserialize_op(const uint8_t* data, size_t len);
std::optional<LoraClientResponseMessage>  deserialize_resp(const uint8_t* data, size_t len);

// An uplink sealed the way a real node seals it: AUTH = SESSION under the K_enc
// / K_mac of (session_id, node_nonce, hub = header.destAddress, node =
// header.senderAddress); SESSION_OPENING when header.sessionNonce != 0; an
// Ack carries FLAGS.ACK + ACKED_FCNT = ack.ack_msg_id.
std::vector<uint8_t> seal_resp(const LoraClientResponseMessage& m, uint32_t session_id,
                               uint32_t node_nonce);
// The same, as the EncryptedPayload of an outer Encrypted message whose header
// is m.header: serialize_resp(outer) is then exactly seal_resp(m, ...), and a
// test can still flip a ciphertext / tag byte or change the outer header.
EncryptedPayload seal_resp_payload(const LoraClientResponseMessage& m, uint32_t session_id,
                                   uint32_t node_nonce);
// A body PDU (already packed) sealed on `port` under header h, for a test that
// builds the body itself (no FLAGS.ACK).
EncryptedPayload seal_resp_body(const LoraHeader& h, const std::vector<uint8_t>& body, uint8_t port,
                                uint32_t session_id, uint32_t node_nonce);
// For the sim models' own sealed uplinks: the PORT / ACK fields of `e` that the
// inner message implies, and the body read back by those fields.
void route_payload(const LoraClientResponseMessage& inner, EncryptedPayload* e);
std::optional<LoraClientResponseMessage> deserialize_resp_payload(const EncryptedPayload& e,
                                                                  const uint8_t* data, size_t len);
// A hub downlink opened with the node's keys (tag checked first); nullopt when
// it is not a SESSION frame those keys verify.
std::optional<LoraClientOperationMessage> open_op(const std::vector<uint8_t>& frame,
                                                  uint32_t session_id, uint32_t node_nonce);

// Convenience constructors used by senders and synthesized-frame tests.
struct AirFrame;  // fwd
AirFrame make_op_frame(const LoraClientOperationMessage& m);
AirFrame make_resp_frame(const LoraClientResponseMessage& m);

// Inspection helpers — return nullopt on parse failure or wrong direction.
std::optional<LoraClientOperationMessage> as_op(const AirFrame& f);
std::optional<LoraClientResponseMessage>  as_resp(const AirFrame& f);

} // namespace proto_sim
