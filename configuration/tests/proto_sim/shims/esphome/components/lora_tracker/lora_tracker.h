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

// Mirrors production: namespace scope, not nested — lora_client.h only
// forward-declares LORATracker.
struct TxPolicy {
    int      copies{0};
    uint32_t stride_ms{0};
};


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

    // Mirrors the production TxPolicy: the copy count belongs to the FRAME,
    // not to the tracker. The old global setBurstCopies() meant any command
    // sent during a drift test went out as a single copy.

    // B1 grid anchor — mirrors production. Set once, never moved.
    void    startGrid();
    int64_t gridAnchorUs() const { return grid_anchor_us_; }
    bool    gridStarted() const  { return grid_started_; }
    int64_t nextT0ForSlotUs(uint8_t slot, int64_t now_us) const;
    uint32_t msUntilNextT0(uint8_t slot) const;
    // The sim's "now" for grid arithmetic. Production reads esp_timer_get_time();
    // a test sets this instead so the answer is deterministic.
    int64_t sim_now_us{0};
    int64_t grid_anchor_us_{0};
    bool    grid_started_{false};

    // Hub→node TX: emits an AirFrame{HubToNode, bytes} into the active SimRadio.
    void send(uint8_t* data, size_t len) { send(data, len, TxPolicy{}); }
    void send(uint8_t* data, size_t len, const TxPolicy& policy);

    // Per-frame record of how each send was requested, so tests can assert that
    // a normal command is never silently reduced to one copy.
    std::vector<int> sent_copies;
    int last_copies{0};

    // Emit `copies` AirFrames instead of one, so node-side duplicate handling
    // sees a real burst. OPT-IN: the default of one frame is what every
    // protocol test counts on, and silently multiplying their transcripts by
    // 17 would rewrite their assertions rather than strengthen them. Burst
    // GEOMETRY is covered by sim/air_channel.h, which models time; this is for
    // tests that care about duplicate ADMISSION.
    void set_expand_bursts(bool v) { expand_bursts = v; }
    bool expand_bursts{false};
    int  default_copies{17};

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
