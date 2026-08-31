// Host-side SHIM for lora_tracker.h. The real header pulls in FreeRTOS +
// SX1278 driver; for protocol tests we only need LORATracker::send() and
// LORATracker::register_client() — the rest of the radio HAL is not under
// test here. Hub-side TX is routed into a SimRadio configured by the
// test fixture.
#pragma once

#include "esphome/core/component.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Production's real lora_tracker.h pulls in lora_client.h transitively;
// the cover .cpp expects LORAListener/LORAClient/LORAClientNode to be
// fully visible after this header. Mirror that.
#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_client/lora_client_node.h"

namespace proto_sim { class SimRadio; }

namespace esphome::lora_tracker {

// LORAListener / LORAClient already declared by the includes above; no
// forward decl needed.

namespace shim_hooks {
// Test-side hook for routing send() → SimRadio.
proto_sim::SimRadio* active_radio();
void set_active_radio(proto_sim::SimRadio* r);
} // namespace shim_hooks

class LORATracker : public Component {
public:
    static constexpr int      loraSpreadingFactor   = 7;
    static constexpr int      loraCodingRate        = 8;
    static constexpr int      loraPreambleLengthRx  = 8;
    static constexpr int      loraPreambleLengthTx  = 8;
    static constexpr long     loraSignalBandwidth   = 500000;
    static constexpr int      loraSyncWord          = 0x12;
    static constexpr uint64_t loraPollingTimeout    = 75;
    static constexpr uint8_t  broadcastAddressing   = 0xFF;
    static constexpr uint8_t  subnetAddressing      = 0xFE;

    void setup() override {}
    void dump_config() override {}
    void loop() override {}

    // Hub→node TX: emits an AirFrame{HubToNode, bytes} into the active SimRadio.
    // Drift test: emit one copy instead of the 17-copy burst. Recorded so a
    // test can assert it is RESTORED to 0 afterwards — leaving the hub on
    // single copies would quietly halve downlink reliability for every node.
    void setBurstCopies(int n) { burst_copies = n; }
    int  burst_copies{0};

    void send(uint8_t* data, size_t len);

    void register_client(LORAClient* client);
    void register_listener(LORAListener* listener);

    // For tests that want to inspect the registered list.
    const std::vector<LORAClient*>&  clients()   const { return clients_; }

private:
    std::vector<LORAClient*>   clients_;
    std::vector<LORAListener*> listeners_;
    uint8_t                    app_id_{0};
};

} // namespace esphome::lora_tracker
