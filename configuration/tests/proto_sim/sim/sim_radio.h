#pragma once

#include "sim/messages.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace proto_sim {

// In-memory broadcast medium between one hub and N nodes. Every transmit is
// observed by every receiver, exactly like real LoRa.
//
// Two delivery modes:
//   * Synchronous (default): every send() fires all sinks before returning,
//     just like phase-1 used. Production radio is async but for most
//     scenarios this is fine and keeps tests simple.
//   * Deferred: send() only appends to a queue. Tests call
//     deliver_pending() / drop_next() / drop_pending(predicate) to control
//     timing. Models the real radio's TX→propagation→RX latency, and lets
//     tests cover lost-ACK retry (B4), BaseNonceExchange recovery (B7),
//     and any race where the ACK timing matters.
class SimRadio {
public:
    using Sink = std::function<void(const AirFrame&)>;

    int add_sink(Sink sink) {
        sinks_.push_back(std::move(sink));
        return static_cast<int>(sinks_.size()) - 1;
    }

    void set_deferred(bool deferred) { deferred_ = deferred; }
    bool deferred() const { return deferred_; }

    void send(const AirFrame& frame) {
        transcript_.push_back(frame);
        if (deferred_) {
            pending_.push_back(frame);
        } else {
            dispatch_(frame);
        }
    }

    // Deliver every frame that was in the queue at the START of this
    // call. Frames pushed BY sinks during delivery stay queued for the
    // next deliver_pending() call — this lets tests drop or inspect
    // sink-generated frames before they reach their destination.
    void deliver_pending() {
        size_t n = pending_.size();
        for (size_t i = 0; i < n; ++i) {
            auto f = pending_.front();
            pending_.pop_front();
            dispatch_(f);
        }
    }

    // Deliver exactly one frame. Useful when the test wants to drop the
    // *next* sink-pushed frame before it lands.
    bool deliver_one() {
        if (pending_.empty()) return false;
        auto f = pending_.front();
        pending_.pop_front();
        dispatch_(f);
        return true;
    }

    // Drop the next queued frame. Returns true if a frame was dropped.
    bool drop_next() {
        if (pending_.empty()) return false;
        pending_.pop_front();
        return true;
    }

    // Drop every queued frame matching predicate.
    template <typename Pred>
    size_t drop_pending(Pred pred) {
        size_t n = 0;
        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if (pred(*it)) { it = pending_.erase(it); ++n; }
            else            { ++it; }
        }
        return n;
    }

    size_t pending_count() const { return pending_.size(); }

    const std::vector<AirFrame>& transcript() const { return transcript_; }

    // Test helpers.
    std::vector<AirFrame> hub_to_node_frames() const {
        std::vector<AirFrame> out;
        for (const auto& f : transcript_)
            if (f.dir == AirFrame::Dir::HubToNode) out.push_back(f);
        return out;
    }
    std::vector<AirFrame> node_to_hub_frames() const {
        std::vector<AirFrame> out;
        for (const auto& f : transcript_)
            if (f.dir == AirFrame::Dir::NodeToHub) out.push_back(f);
        return out;
    }

private:
    void dispatch_(const AirFrame& frame) {
        for (auto& sink : sinks_) sink(frame);
    }

    bool                  deferred_{false};
    std::vector<Sink>     sinks_;
    std::vector<AirFrame> transcript_;
    std::deque<AirFrame>  pending_;
};

} // namespace proto_sim
