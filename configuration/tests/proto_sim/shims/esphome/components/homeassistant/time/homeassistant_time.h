// Host-side shim for the ESPHome HomeAssistant time component. Mimics the
// production class's now()/timestamp/is_valid/add_on_time_sync_callback
// surface so lora_client.cpp's NTP-sync gating can be exercised under tests.
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <vector>

namespace esphome::time {

struct ESPTime {
    std::time_t timestamp{0};
    bool valid{false};
    bool is_valid() const { return valid; }
};

class RealTimeClock {
public:
    ESPTime now() const { return now_; }

    void add_on_time_sync_callback(std::function<void()> cb) {
        callbacks_.push_back(std::move(cb));
    }

    // Test helpers.
    void set_now(std::time_t t, bool valid = true) {
        now_.timestamp = t;
        now_.valid     = valid;
    }
    void fire_sync_callbacks() {
        for (auto& cb : callbacks_) cb();
    }

private:
    ESPTime now_{};
    std::vector<std::function<void()>> callbacks_;
};

} // namespace esphome::time
