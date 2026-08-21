#pragma once

#include "sim/messages.h"
#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/wire_codec.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace proto_sim {

// Mirrors LORAListener + LORATracker in local_components/lora_client and
// lora_tracker. One HubListener instance per configured client (rol_1, rol_2,
// ...). They share a HubTracker which owns the SimRadio and dispatches every
// received frame to every listener.
//
// The state machine intentionally matches the production code path-for-path so
// regressions show up as test failures. Where production uses ESPHome scheduler
// names ("login_startup", "login_retry") we use the same names against SimClock.

class HubTracker;

struct HubNvsBlob {
    uint8_t  version{2};
    uint32_t rx_message_id{0};
    uint32_t tx_message_id{0};
    bool     logged_in{false};
    uint32_t last_sleep_epoch{0};
};

// Per-peer base nonce store, shared across all listeners exactly like the
// file-scope s_base_nonce_map static in production.
class SharedNonceMap {
public:
    void   set(uint32_t peer, uint32_t nonce) { map_[peer] = nonce; }
    void   clear()                            { map_.clear(); }
    bool   contains(uint32_t peer) const      { return map_.count(peer) != 0; }
    uint32_t get(uint32_t peer) const         { auto it = map_.find(peer); return it == map_.end() ? 0u : it->second; }
    size_t size() const                       { return map_.size(); }
private:
    std::map<uint32_t, uint32_t> map_;
};

class HubListener {
public:
    HubListener(std::string name, uint8_t short_address, uint8_t subnet_address,
                uint64_t mac_addr, uint32_t sleep_duration_s,
                HubTracker* tracker, SimClock* clock, SharedNonceMap* nonces);

    // ---- configuration ----
    const std::string& name() const { return name_; }
    uint8_t short_address() const { return short_address_; }
    uint64_t mac() const { return mac_; }

    // Inject NVS state before setup() to simulate a warm boot.
    void preload_nvs(const HubNvsBlob& blob) { nvs_ = blob; have_nvs_ = true; }
    void wipe_nvs() { have_nvs_ = false; nvs_ = HubNvsBlob{}; }
    HubNvsBlob nvs_snapshot() const { return nvs_; }

    // ---- lifecycle ----
    void setup(bool time_valid_at_boot);

    // Simulate a hub-side reboot: drops RAM-only state (login flags, frame
    // counters, pending nonce) but keeps the persisted nvs_ blob (unless
    // wipe_nvs() is called first). The caller follows up with another
    // setup() to re-enter the boot path.
    void simulate_reboot();

    // ---- RX dispatch (called by HubTracker) ----
    void on_frame(const AirFrame& frame);

    // ---- observable state ----
    bool registered() const { return registered_; }
    bool login_acked() const { return login_acked_; }
    uint8_t login_retry_count() const { return login_retry_count_; }
    uint32_t pending_login_nonce() const { return pending_login_nonce_; }
    uint32_t tx_message_id() const { return tx_message_id_; }
    uint32_t rx_message_id() const { return rx_message_id_; }
    uint32_t nvs_write_count() const { return nvs_write_count_; }
    void reset_nvs_write_count() { nvs_write_count_ = 0; }

    // ---- actions that production code exposes ----
    void send_login();
    void send_remote_config();
    void enter_sleep();

    // Recovery path: hub sends a fresh 4-byte base nonce as a standalone
    // CMD_BASENONCE message. Production calls this when an encrypted reply
    // arrives for a peer the hub has no nonce for (file-scope map cleared
    // by hub reboot).
    void send_base_nonce_exchange();

    // Cover-side TX (mirrors LoraCoverComponent::control). The destAddress is
    // ALWAYS taken from this listener's short_address_ — that's the invariant
    // the user's original misrouting suspicion was about.
    void send_cover_op(CovOperation op);
    void send_cover_position(float position);
    void send_sysop(ClientOperation op);   // CMD_OTA / WIFI / STATUS
    void send_cover_config(uint32_t open_time, uint32_t close_time,
                           float height_mm, float axle_mm, float thickness_mm);

private:
    void do_login_and_arm_retry_();
    void schedule_startup_login_();
    void persist_nvs_();
    void send_op_(LoraClientOperationMessage msg);

    // Pluggable RNG so tests can pin the nonce sequence.
public:
    std::function<uint32_t()> rng = [] { return 0xDEADBEEFu; };

private:
    std::string     name_;
    uint8_t         short_address_;
    uint8_t         subnet_address_;
    uint64_t        mac_;
    uint32_t        sleep_duration_s_;

    HubTracker*     tracker_;
    SimClock*       clock_;
    SharedNonceMap* nonces_;

    HubNvsBlob      nvs_{};
    bool            have_nvs_{false};

    bool            registered_{false};
    bool            login_acked_{false};
    uint8_t         login_retry_count_{0};
    bool            startup_login_initiated_{false};
    uint32_t        pending_login_nonce_{0};

    uint32_t        nvs_write_count_{0};
    uint32_t        tx_message_id_{0};
    uint32_t        rx_message_id_{0};

public:
    static constexpr uint8_t  kMaxLoginRetries  = 24;
    static constexpr uint32_t kLoginStaggerMs   = 3000;
    static constexpr uint32_t kNodeBootMarginMs = 5000;
    static constexpr uint32_t kRetryIntervalMs  = 3600000;
};

class HubTracker {
public:
    HubTracker(SimClock* clock, SimRadio* radio, SharedNonceMap* nonces)
        : clock_(clock), radio_(radio), nonces_(nonces) {
        radio_->add_sink([this](const AirFrame& f) { on_air(f); });
    }

    void register_listener(HubListener* l) { listeners_.push_back(l); }

    void send(LoraClientOperationMessage msg) {
        radio_->send(make_op_frame(msg));
    }

    SimClock* clock() const { return clock_; }
    SharedNonceMap* nonces() const { return nonces_; }

private:
    void on_air(const AirFrame& f) {
        if (f.dir != AirFrame::Dir::NodeToHub) return;
        for (auto* l : listeners_) l->on_frame(f);
    }

    SimClock*       clock_;
    SimRadio*       radio_;
    SharedNonceMap* nonces_;
    std::vector<HubListener*> listeners_;
};

} // namespace proto_sim
