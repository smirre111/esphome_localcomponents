#pragma once
// v1_client.h -- the listener as the v0-era listener tests drive it.
//
// S8 / W4: with MAC_WIRE_V1 (the default) the hub accepts only v1 frames. The
// listener tests build node uplinks as v0 frames; TestClient translates each
// one at the set_response boundary (sim/v1_bridge.h) so the REAL hub code runs
// on the v1 frame a real node would have sent. With MAC_WIRE_V1=0 it is the
// plain client. Include this INSTEAD of naming esphome::lora_tracker::LORAClient.

#include "esphome/components/lora_client/lora_client.h"
#include "sim/v1_bridge.h"

namespace proto_sim {

#if defined(MAC_WIRE_V1) && MAC_WIRE_V1
class TestClient : public esphome::lora_tracker::LORAClient {
public:
    void set_response(uint8_t *data, size_t len) {
        std::vector<uint8_t> v1 = v1bridge::uplinkToV1(data, len);
        esphome::lora_tracker::LORAClient::set_response(v1.data(), v1.size());
    }
};
#else
using TestClient = esphome::lora_tracker::LORAClient;
#endif

}  // namespace proto_sim
