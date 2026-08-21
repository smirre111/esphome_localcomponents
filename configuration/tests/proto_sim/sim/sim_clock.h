#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace proto_sim {

// Virtual clock + named scheduler that mirrors ESPHome's
// Component::set_timeout / set_interval / cancel_* semantics.
//
// Test code is the only thing that advances time, via tick(ms) or
// advance_until(condition).  Callbacks fire in due-time order; for ties the
// order of registration is preserved (FIFO).
//
// Reproduces one subtle behaviour we rely on in production code:
//  * set_timeout(name, ...) and set_interval(name, ...) keyed by name
//    REPLACE the previous entry with the same name (so re-arming cancels
//    the old one).
//  * cancel_timeout / cancel_interval remove the entry; entries already due
//    in the same tick(ms) call still fire if they were due before cancel.
//    (That is the race we want to be able to reproduce — see scenarios/login_test.)
class SimClock {
public:
    using Callback = std::function<void()>;

    SimClock() = default;

    // Absolute virtual time in ms since "boot".
    uint32_t now_ms() const { return now_; }

    void set_timeout(const std::string& name, uint32_t delay_ms, Callback cb);
    void set_interval(const std::string& name, uint32_t period_ms, Callback cb);
    void cancel_timeout(const std::string& name);
    void cancel_interval(const std::string& name);

    // Advance virtual time by dt_ms, firing any callbacks whose due time
    // falls within [now_, now_+dt_ms].  Callbacks may schedule further work;
    // that work fires on subsequent tick() calls (no recursive flush).
    void tick(uint32_t dt_ms);

private:
    struct Entry {
        uint32_t due_ms;
        uint32_t period_ms; // 0 → one-shot timeout
        Callback cb;
        uint64_t insertion_seq;
    };

    uint32_t now_{0};
    uint64_t seq_{0};
    std::map<std::string, Entry> entries_;
};

} // namespace proto_sim
