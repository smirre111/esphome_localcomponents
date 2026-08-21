// Host-side shim for the ESPHome HomeAssistant time component. Mimics the
// production class's now()/timestamp/is_valid/add_on_time_sync_callback
// surface so lora_client.cpp's NTP-sync gating can be exercised under tests.
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <vector>

// Production puts ESPTime in namespace `esphome` (core/time.h) and
// RealTimeClock in `esphome::time`. Mirror that split exactly, otherwise
// production code that qualifies esphome::ESPTime::timezone_offset() will not
// compile against the shim.
namespace esphome {

struct ESPTime {
    std::time_t timestamp{0};
    bool valid{false};
    bool is_valid() const { return valid; }

    // Local UTC offset in seconds, including DST. Production reads this from
    // the configured timezone; tests set it directly.
    static int32_t timezone_offset() { return tz_offset_; }
    static void set_timezone_offset(int32_t seconds) { tz_offset_ = seconds; }

    static inline int32_t tz_offset_{0};
};

} // namespace esphome

namespace esphome::time {

using ESPTime = ::esphome::ESPTime;

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
