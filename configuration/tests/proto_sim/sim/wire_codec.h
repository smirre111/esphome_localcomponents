#pragma once

#include "sim/messages.h"

#include <cstdint>
#include <optional>
#include <vector>

// Phase-2 codec: translates the proto_sim plain-struct messages.h types to and
// from real wire bytes using the generated `local_components/blindsproto/
// blinds.pb-c.{h,c}` stubs and the protobuf-c runtime.
//
// Keeping the codec in its own TU (no `using namespace proto_sim` in
// wire_codec.cpp) avoids the name collision between our proto_sim::*
// structs and the C global-namespace symbols protobuf-c generates.

namespace proto_sim {

std::vector<uint8_t> serialize_op(const LoraClientOperationMessage& m);
std::vector<uint8_t> serialize_resp(const LoraClientResponseMessage& m);

// Payload-only variants: pack WITHOUT the LoraHeader.  Production encrypts the
// payload only — the inner message's header is redundant because the receiver
// uses the plaintext outer header — so these are what goes into the AEAD
// ciphertext.  See CmdDispatcher.cpp:533 and lora_client.cpp's matching unwrap.
std::vector<uint8_t> serialize_op_payload(const LoraClientOperationMessage& m);
std::vector<uint8_t> serialize_resp_payload(const LoraClientResponseMessage& m);

// Returns std::nullopt on parse failure (the production "Could not read
// protobuf" path). Tests for H2/H3 rely on this.
std::optional<LoraClientOperationMessage> deserialize_op(const uint8_t* data, size_t len);
std::optional<LoraClientResponseMessage>  deserialize_resp(const uint8_t* data, size_t len);

// Convenience constructors used by senders and synthesized-frame tests.
struct AirFrame;  // fwd
AirFrame make_op_frame(const LoraClientOperationMessage& m);
AirFrame make_resp_frame(const LoraClientResponseMessage& m);

// Inspection helpers — return nullopt on parse failure or wrong direction.
std::optional<LoraClientOperationMessage> as_op(const AirFrame& f);
std::optional<LoraClientResponseMessage>  as_resp(const AirFrame& f);

} // namespace proto_sim
