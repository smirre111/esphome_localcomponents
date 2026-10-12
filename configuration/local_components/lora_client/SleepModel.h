#pragma once

#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------------------
// SleepModel -- what the hub believes about an interactive node's sleep, and
// what it does about it.
//
// USER DECISION (2026-10-12): "a command issued while the node is asleep shall
// be IGNORED (as the node is asleep)". Before, the hub sent it anyway: five
// frames, a failed retry ladder and a forced re-login to a node that was not
// listening.
//
// THE PREDICATE (all of it, so the edge is documented in one place). The hub
// calls a node ASLEEP when, and only when:
//   * it is not an automatic-mode node (those schedule their own sleep and are
//     reached through pending-data / their Class A windows; the hub models no
//     "sleep ack" for them, and their flow must not change);
//   * its last SLEEP was not left UNCONFIRMED (no ack: the node may or may not
//     have slept, so it is treated as awake and commands go out);
//   * a SLEEP was acknowledged (last_sleep_epoch != 0) and the node has not been
//     heard since (last_heard < last_sleep) -- hearing it, for any reason (an
//     early reboot, a REGISTER), is proof it is awake and beats the clock;
//   * LORAListener::is_node_awake_() says "not awake" -- the existing model, the
//     only definition of the wake time (last_sleep + sleep_duration). It is used
//     through ms_until_node_awake_(), which is 0 exactly when is_node_awake_()
//     holds (and when the hub cannot tell: no clock, nothing on record), so the
//     one number below carries both answers;
//   * and the wake is further away than the boot margin below.
//
// THE EDGE. The node wakes about sleep_duration after the hub stamped the sleep
// ack, then needs its boot (the 5 s NODE_BOOT_MARGIN the post-sleep login
// fallback already waits) before it listens, and the hub staggers the nodes'
// logins by 3 s per YAML slot. The hub therefore treats the node as awake
// slightly BEFORE the modelled wake: asleep ends marginMs(slot) ahead of it.
// An op issued inside that margin is sent and its normal ack ladder spans the
// boot; one issued earlier is ignored. The same instant turns the diagnostic
// "Asleep" sensor off.
// ---------------------------------------------------------------------------

namespace sleepmodel
{

constexpr uint32_t kBootMarginMs   = 5000;   // = NODE_BOOT_MARGIN_MS of the login fallback
constexpr uint32_t kLoginStaggerMs = 3000;   // = LOGIN_STAGGER_MS per YAML slot

constexpr uint32_t marginMs(uint8_t login_slot)
{
    return kBootMarginMs + static_cast<uint32_t>(login_slot) * kLoginStaggerMs;
}

enum class State : uint8_t
{
    Awake,
    Asleep,        // a sleep was acknowledged and the modelled wake is still ahead
    Unconfirmed,   // a SLEEP got no ack: treated as awake, commands go out
    Auto,          // automatic mode: the node schedules its own sleep, nothing is modelled
};

struct Inputs
{
    bool     auto_mode;
    bool     sleep_unconfirmed;
    uint32_t last_sleep_epoch;   // 0: no acknowledged sleep on record
    uint32_t last_heard_epoch;
    uint32_t ms_until_awake;     // LORAListener::ms_until_node_awake_(): 0 = awake by the model
    uint32_t margin_ms;
};

constexpr State classify(const Inputs &in)
{
    if (in.auto_mode)
        return State::Auto;
    if (in.sleep_unconfirmed)
        return State::Unconfirmed;
    if (in.last_heard_epoch >= in.last_sleep_epoch)   // also: no sleep on record (0)
        return State::Awake;
    if (in.ms_until_awake <= in.margin_ms)
        return State::Awake;
    return State::Asleep;
}

// How long until the state must be looked at again so "Asleep" ends on time: the
// moment the margin opens, plus a second so the comparison is strictly past it.
constexpr uint32_t refreshDelayMs(uint32_t ms_until_awake, uint32_t margin_ms)
{
    return (ms_until_awake > margin_ms ? ms_until_awake - margin_ms : 0u) + 1000u;
}

// Local time of day of an epoch second, for log lines and the status text.
inline void clockHm(uint32_t epoch, int32_t tz_offset_s, unsigned *h, unsigned *m)
{
    int64_t s = (static_cast<int64_t>(epoch) + tz_offset_s) % 86400;
    if (s < 0)
        s += 86400;
    *h = static_cast<unsigned>(s / 3600);
    *m = static_cast<unsigned>((s % 3600) / 60);
}

// The text sensor's value.
inline void statusText(State s, uint32_t wake_epoch, int32_t tz_offset_s, char *buf, size_t n)
{
    switch (s)
    {
    case State::Asleep:
    {
        unsigned h = 0, m = 0;
        clockHm(wake_epoch, tz_offset_s, &h, &m);
        std::snprintf(buf, n, "asleep until %02u:%02u", h, m);
        return;
    }
    case State::Unconfirmed:
        std::snprintf(buf, n, "sleep unconfirmed");
        return;
    case State::Auto:
        std::snprintf(buf, n, "auto mode");
        return;
    default:
        std::snprintf(buf, n, "awake");
        return;
    }
}

}  // namespace sleepmodel
