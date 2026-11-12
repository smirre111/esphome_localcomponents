#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// SessionDiag -- what happened to one node's session, kept for the operator.
// HUB-ONLY, dependency-free (host test: tests/proto_sim/scenarios/
// session_diag_test.cpp).
//
// WHY. Every slow REGISTER / LOGIN recovery of 2026-11 was found by reading a
// serial capture afterwards and counting LoginMsg lines by hand; node 1 has no
// serial at all. The hub already sees every fact needed to say "this node is
// transmitting and I still have no session with it" -- it only never said so.
//
// THE STALE PERIOD (the one predicate, so the edge is documented in one place).
// A node is STALE when the hub has heard it WITHOUT a session -- an encrypted
// uplink it holds no keys for, an uplink with no session id on record, or an
// admitted REGISTER -- and no session was confirmed within kStaleAfterMs of
// the FIRST such uplink. It ends at the next confirmed session or when the
// node is put to sleep. A node that is silent is never stale: a sleeping node
// and a node out of range look the same from here, and neither is this fault.
// The healthy REGISTER -> LOGIN exchange takes ~6 s (hardware, 2026-11-06), so
// 30 s is five of those and well clear of it.
//
// A keyless uplink is UNAUTHENTICATED: a forged frame can raise the flag and
// the counter. That is acceptable for a diagnostic (nothing acts on it) and is
// the same exposure the keyless LOGIN re-send already has.
//
// THE RECOVERY TIME. From the first sign that a session is needed (a LOGIN
// sent, a node heard without a session, a session dropped after an unacked
// command) to the confirmation that follows. Sleep cancels the measurement:
// a session ended on purpose is not a recovery.
//
// Time is a monotonic millisecond count supplied by the caller (0 is a valid
// instant; "not running" is carried by flags, never by a zero).
// ---------------------------------------------------------------------------

namespace sessiondiag
{

constexpr uint32_t kStaleAfterMs = 30000;

enum class State : uint8_t
{
    NoSession,      // no keys and no challenge in flight
    LoginPending,   // a LOGIN challenge is in flight (or due) and not yet proven
    Confirmed,      // the node proved the session
    Asleep,         // the hub models the node as asleep (SleepModel.h)
};

constexpr State classify(bool asleep, bool confirmed, bool challenge_in_flight)
{
    return asleep ? State::Asleep
                  : confirmed ? State::Confirmed
                              : challenge_in_flight ? State::LoginPending : State::NoSession;
}

// The text sensor's value. `stale` only qualifies the two session-less states.
constexpr const char *stateText(State s, bool stale)
{
    return s == State::Asleep ? "asleep"
           : s == State::Confirmed ? "confirmed"
           : stale ? "stale (heard, no session)"
           : s == State::LoginPending ? "login pending" : "no session";
}

enum class RegisterRefusal : uint8_t
{
    BadTag,        // the K_AUTH tag did not verify
    Replayed,      // register_seq not above the highest seen (HB14)
    RateLimited,   // inside the REGISTER rate limit
};

constexpr const char *refusalText(RegisterRefusal r)
{
    return r == RegisterRefusal::BadTag ? "bad tag"
           : r == RegisterRefusal::Replayed ? "replayed register_seq" : "rate-limited";
}

// Since hub boot. All saturate nowhere: 2^32 events is not reachable.
struct Counters
{
    uint32_t logins_sent{0};          // LoginMsg frames handed to MAC-0
    uint32_t registers_accepted{0};
    uint32_t registers_refused{0};    // authentic-looking REGISTERs for this node that were dropped
    uint32_t keyless_uplinks{0};      // uplinks heard without a usable session
    uint32_t sessions_dropped{0};     // a confirmed session given up after an unacked command
    uint32_t stale_periods{0};
    uint32_t recoveries{0};           // recovery times measured
};

class Tracker
{
public:
    // ---- events ----
    // A LoginMsg went to MAC-0 (first send or retry).
    void noteLoginSent(int64_t now_ms)
    {
        ++c_.logins_sent;
        ++recovery_logins_;
        startRecovery_(now_ms);
    }
    // An uplink the hub could not use for lack of a session.
    void noteKeylessUplink(int64_t now_ms)
    {
        ++c_.keyless_uplinks;
        noteHeardWithoutSession_(now_ms);
    }
    void noteRegisterAccepted(int64_t now_ms)
    {
        ++c_.registers_accepted;
        noteHeardWithoutSession_(now_ms);
    }
    void noteRegisterRefused(RegisterRefusal why)
    {
        ++c_.registers_refused;
        last_refusal_     = why;
        have_refusal_     = true;
    }
    // A confirmed session was given up (a command went fully unacked).
    void noteSessionDropped(int64_t now_ms)
    {
        ++c_.sessions_dropped;
        startRecovery_(now_ms);
    }
    // The node proved the session. Returns true when this ended a recovery
    // (lastRecoveryMs / lastRecoveryLogins describe it).
    bool noteConfirmed(int64_t now_ms)
    {
        heard_          = false;
        stale_latched_  = false;
        if (!recovering_)
            return false;
        recovering_          = false;
        last_recovery_ms_    = elapsed_(recovery_since_ms_, now_ms);
        last_recovery_logins_ = recovery_logins_;
        recovery_logins_     = 0;
        ++c_.recoveries;
        return true;
    }
    // The node was put to sleep: the session ended on purpose.
    void noteAsleep()
    {
        heard_           = false;
        stale_latched_   = false;
        recovering_      = false;
        recovery_logins_ = 0;
    }

    // ---- the stale period ----
    // True while the node is stale. Counts each period once, at the first call
    // that finds it: returns through *became when that call is this one.
    bool poll(int64_t now_ms, bool *became = nullptr)
    {
        const bool s = heard_ && elapsed_(heard_since_ms_, now_ms) >= kStaleAfterMs;
        const bool first = s && !stale_latched_;
        if (first)
        {
            stale_latched_ = true;
            ++c_.stale_periods;
        }
        if (became != nullptr)
            *became = first;
        return s;
    }
    bool stale(int64_t now_ms) const
    {
        return heard_ && elapsed_(heard_since_ms_, now_ms) >= kStaleAfterMs;
    }
    // Milliseconds until poll() must run again to raise the flag on time; 0 when
    // no clock is running or the node is already stale (nothing to wait for).
    uint32_t msUntilStale(int64_t now_ms) const
    {
        if (!heard_)
            return 0;
        const uint32_t e = elapsed_(heard_since_ms_, now_ms);
        return e >= kStaleAfterMs ? 0u : kStaleAfterMs - e;
    }
    // How long the node has been heard without a session (0: not at all).
    uint32_t heardWithoutSessionMs(int64_t now_ms) const
    {
        return heard_ ? elapsed_(heard_since_ms_, now_ms) : 0u;
    }

    // ---- read-out ----
    const Counters &counters() const { return c_; }
    bool     recovering() const { return recovering_; }
    uint32_t recoveryLogins() const { return recovery_logins_; }   // of the recovery in progress
    uint32_t lastRecoveryMs() const { return last_recovery_ms_; }  // valid when counters().recoveries != 0
    uint32_t lastRecoveryLogins() const { return last_recovery_logins_; }
    bool     hasRefusal() const { return have_refusal_; }
    RegisterRefusal lastRefusal() const { return last_refusal_; }

private:
    static uint32_t elapsed_(int64_t since_ms, int64_t now_ms)
    {
        const int64_t d = now_ms - since_ms;
        return d <= 0 ? 0u : d > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<uint32_t>(d);
    }
    void startRecovery_(int64_t now_ms)
    {
        if (recovering_)
            return;
        recovering_        = true;
        recovery_since_ms_ = now_ms;
    }
    void noteHeardWithoutSession_(int64_t now_ms)
    {
        startRecovery_(now_ms);
        if (heard_)
            return;   // the clock runs from the FIRST such uplink
        heard_          = true;
        heard_since_ms_ = now_ms;
    }

    Counters c_{};
    bool     heard_{false};
    int64_t  heard_since_ms_{0};
    bool     stale_latched_{false};
    bool     recovering_{false};
    int64_t  recovery_since_ms_{0};
    uint32_t recovery_logins_{0};
    uint32_t last_recovery_ms_{0};
    uint32_t last_recovery_logins_{0};
    bool            have_refusal_{false};
    RegisterRefusal last_refusal_{RegisterRefusal::BadTag};
};

}  // namespace sessiondiag
