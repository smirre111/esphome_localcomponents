#pragma once

#include "sim/messages.h"
#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/wire_codec.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace proto_sim {

// Mirrors BlindsESP/main/CmdDispatcher.cpp. One NodeModel per physical node.

class NodeModel {
public:
    NodeModel(std::string label, uint64_t mac, SimClock* clock, SimRadio* radio);

    // ---- visibility for assertions ----
    uint8_t  cfg_address() const { return cfg_address_; }
    uint8_t  cfg_subnet()  const { return cfg_subnet_; }
    bool     registered() const { return registered_; }
    uint32_t tx_message_id() const { return tx_message_id_; }
    uint32_t rx_message_id() const { return rx_message_id_; }
    uint32_t base_nonce_for(uint32_t peer) const {
        auto it = peer_base_.find(peer); return it == peer_base_.end() ? 0u : it->second;
    }
    const std::string& last_motor_cmd() const { return last_motor_cmd_; }
    const std::vector<AirFrame>& outbox() const { return outbox_; }

    // CoverConfig state observed by the node. open_time/close_time are
    // applied unconditionally; the three geometry floats are applied only
    // when ALL THREE are non-zero (proto3 unset == 0). geometry_applied()
    // reports whether the most recent CoverConfig hit the all-non-zero arm
    // of the production guard.
    uint32_t open_time_s()  const { return open_time_s_; }
    uint32_t close_time_s() const { return close_time_s_; }
    float    height_mm()    const { return height_mm_; }
    float    axle_mm()      const { return axle_mm_; }
    float    thickness_mm() const { return thickness_mm_; }
    bool     geometry_applied() const { return geometry_applied_; }

    // ---- inject state ----
    void set_cfg_address(uint8_t addr, uint8_t subnt) { cfg_address_ = addr; cfg_subnet_ = subnt; }
    void set_registered(bool v) { registered_ = v; }

    // ---- node-driven actions ----
    void send_register();
    void send_available();
    void send_position(float position);

    // Simulate a node-side cold boot: clears RAM-only state but keeps cfg if
    // explicitly preserved (test owns whether LittleFS survives).
    void reboot(bool keep_cfg);

    // ---- RX entry point ----
    void on_frame(const AirFrame& f);

private:
    void send_resp_(LoraClientResponseMessage msg);

    std::string label_;
    uint64_t    mac_;

    SimClock*   clock_;
    SimRadio*   radio_;

    uint8_t     cfg_address_{0};
    uint8_t     cfg_subnet_{0};
    bool        registered_{false};

    uint32_t    tx_message_id_{0};
    uint32_t    rx_message_id_{0};
    uint32_t    last_hub_addr_{0xFF};

    std::map<uint32_t, uint32_t> peer_base_;

    // Production rate-limits CMD_LOGIN to one accepted per 5 s.
    // login_seen_once_ is a sentinel so an initial last_ms_=0 at t=0 isn't
    // mistaken for "never accepted".
    bool        login_seen_once_{false};
    uint32_t    last_login_accepted_ms_{0};
    static constexpr uint32_t kLoginRateLimitMs = 5000;

    std::string last_motor_cmd_{};
    std::vector<AirFrame> outbox_;

    uint32_t open_time_s_{0};
    uint32_t close_time_s_{0};
    float    height_mm_{0.0f};
    float    axle_mm_{0.0f};
    float    thickness_mm_{0.0f};
    bool     geometry_applied_{false};
};

} // namespace proto_sim
