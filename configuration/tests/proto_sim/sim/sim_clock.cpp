#include "sim/sim_clock.h"

#include <algorithm>

namespace proto_sim {

void SimClock::set_timeout(const std::string& name, uint32_t delay_ms, Callback cb) {
    entries_[name] = Entry{now_ + delay_ms, 0, std::move(cb), seq_++};
}

void SimClock::set_interval(const std::string& name, uint32_t period_ms, Callback cb) {
    entries_[name] = Entry{now_ + period_ms, period_ms, std::move(cb), seq_++};
}

void SimClock::cancel_timeout(const std::string& name) {
    entries_.erase(name);
}

void SimClock::cancel_interval(const std::string& name) {
    entries_.erase(name);
}

void SimClock::tick(uint32_t dt_ms) {
    const uint32_t target = now_ + dt_ms;

    // Repeatedly find the next-due callback within [now_, target] and fire it.
    // We re-scan after each fire because the callback may have scheduled new
    // entries or cancelled existing ones.
    while (true) {
        const Entry* next = nullptr;
        std::string next_name;

        for (const auto& [name, entry] : entries_) {
            if (entry.due_ms > target) continue;
            if (!next || entry.due_ms < next->due_ms ||
                (entry.due_ms == next->due_ms && entry.insertion_seq < next->insertion_seq)) {
                next = &entry;
                next_name = name;
            }
        }

        if (!next) break;

        now_ = next->due_ms;
        Callback cb = next->cb;          // copy so re-arm/cancel inside cb is safe
        uint32_t period = next->period_ms;

        if (period == 0) {
            entries_.erase(next_name);
        } else {
            // Re-arm under the same name for the next period.
            entries_[next_name].due_ms = now_ + period;
        }
        cb();
    }

    now_ = target;
}

} // namespace proto_sim
