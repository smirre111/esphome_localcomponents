#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// ModeTest — the on-hardware validation mode's DECISIONS.
//
// Everything here is a rule that must hold whether or not a radio is attached:
// how long a test may run, when a node must refuse to arm one, which grid
// periods are measurable, and how a distribution is summarised. The wiring —
// timers, queues, the report frame — lives in CmdDispatcher; this is what the
// host suite can pin.
//
// Specified in docs/test-plan.md section 10. Two things there are load-bearing
// and are the reason this file exists rather than a handful of literals in a
// task body:
//
//   * ARMING IS AUTHENTICATED EVEN THOUGH THE TRAFFIC IS NOT. ModeTest{enable}
//     travels the normal application path, but the frames it then produces need
//     no session (MAC-0 is the configuration every node passes through on every
//     cold boot, before the base-nonce exchange). So a node with no session must
//     REFUSE to arm, or the unauthenticated bootstrap window becomes a way to
//     park 32 nodes in a test mode.
//
//   * DISTRIBUTIONS, NOT MEANS. Section 12.3 flags the RxDone ISR latency as the
//     measurement most likely to fail, precisely because production scales
//     40-240 MHz and the motor's PM lock changes the frequency mid-operation —
//     so PLL relock time varies with what the node is doing. A mean would hide
//     exactly that, which is why Hist carries p95 and p99 and max.
//
// Dependency-free by policy: <stdint.h> only, so the host suite compiles it
// straight from the node source and a schema-drift gate can compare the two
// copies byte for byte.
// ---------------------------------------------------------------------------

namespace modetest {

// --- Which mode the test drives -------------------------------------------
//
// Mirrors ModeTest.Mode on the wire. Unspec means "leave the node's mode
// alone", which is the safe default: a test that forces a mode must also put it
// back, and one that forces nothing has nothing to restore.
enum class Mode : uint8_t {
    Unspec = 0,
    A      = 1,
    B      = 2,
    C      = 3,
    Sweep  = 4,
};

// --- Duration --------------------------------------------------------------
//
// The NODE owns this, not the hub. Every task loop blocks on its queue with
// portMAX_DELAY, so a poll would not end the test; a one-shot esp_timer does,
// independently of whether the hub is still there. Same shape as
// drift::testDurationS, and deliberately a shorter ceiling: ModeTest can hold
// the radio in continuous RX AND force a mode, so an abandoned run costs more
// than an abandoned drift test.
static constexpr uint32_t kDefaultDurationS = 300;    // 5 min
static constexpr uint32_t kMaxDurationS     = 900;    // 15 min

constexpr uint32_t testDurationS(uint32_t requested)
{
    const uint32_t s = requested ? requested : kDefaultDurationS;
    return s > kMaxDurationS ? kMaxDurationS : s;
}

// --- Grid period -----------------------------------------------------------
//
// A grid whose period is commensurate with the node's RX interval PHASE-LOCKS:
// the frames keep a constant phase relationship with the windows, so a frame
// that lands in an RX-off gap does so forever. This is not hypothetical — it
// was observed on DriftTest at a 1000 ms grid against 500 ms windows: ~300 grid
// frames sent, zero heard. DriftTest uses 1100 ms for exactly this reason.
//
// MODE_B inverts it. There the period must be exactly the round and phase-locked
// to the node's slot; that IS the mode. So the rule applies to MODE_A and
// MODE_SWEEP only, and it is enforced at arm time rather than left for an
// operator to rediscover.
constexpr bool periodsAreCommensurate(uint32_t grid_period_ms, uint32_t rx_interval_ms)
{
    if (grid_period_ms == 0 || rx_interval_ms == 0) return false;
    const uint32_t a = (grid_period_ms > rx_interval_ms) ? grid_period_ms : rx_interval_ms;
    const uint32_t b = (grid_period_ms > rx_interval_ms) ? rx_interval_ms : grid_period_ms;
    return (a % b) == 0;
}

constexpr bool periodMattersFor(Mode m)
{
    return m == Mode::A || m == Mode::Sweep || m == Mode::Unspec;
}

// --- Arming ----------------------------------------------------------------

enum class ArmRefusal : uint8_t {
    None = 0,
    NotAuthenticated,   // the arming frame was plaintext — see NodeContext
    NoSession,          // no session at all — see the banner
    SweepOffBench,      // MODE_SWEEP deliberately mis-arms windows
    CommensurateGrid,   // would measure nothing; see periodsAreCommensurate
    BatteryTooLow,      // a test that flattens the node teaches nothing
    BadCopies,          // outside 1..17, so not a burst this fleet ever sends
    NoGridPeriod,       // a mode that needs a ruler was given none
};

// What the node knows about itself when the arm request lands.
struct NodeContext {
    // Did the ARMING frame itself arrive authenticated?
    //
    // mac-layer.md §4 states this is not optional: "a plaintext frame asking to
    // turn authentication off answers its own question". MacSublayers::armRefusal
    // has carried the equivalent parameter from the start; this one did not, and
    // handleModeTest then wrote sublayers_.counter_enabled and
    // sublayers_.crypto_enabled — the exact two variables applyMacConfig_ guards
    // — from proto3 fields that default to false. A plaintext ModeTest against a
    // node with a live session disabled MAC-1 and MAC-2 and, because
    // keepPowerProfile is a proto3 bool defaulting to false despite its comment
    // saying "DEFAULT TRUE", pinned the CPU at 240 MHz with light sleep off.
    //
    // Defaults to false, so a NodeContext that forgets to set it refuses rather
    // than arms — the same direction as every other default in these headers.
    bool     frame_authenticated{false};
    bool     has_session{false};
    bool     is_bench_node{false};
    uint32_t battery_mv{4000};
    uint32_t rx_interval_ms{500};
};

// The request, as far as the decision is concerned.
struct Request {
    Mode     mode{Mode::Unspec};
    uint32_t grid_period_ms{0};
    uint32_t copies{1};
    uint32_t duration_s{0};
};

// Below this the node refuses to start. A ModeTest can hold the radio in
// continuous RX for fifteen minutes; running one on a nearly flat cell buys a
// number and loses the node.
static constexpr uint32_t kMinBatteryMv = 3500;

// The hub never sends more than txSlotsPerRound copies, so a request outside
// 1..17 is not a burst this fleet produces and the measurement would not
// describe it.
static constexpr uint32_t kMinCopies = 1;
static constexpr uint32_t kMaxCopies = 17;

constexpr ArmRefusal armRefusal(const Request &r, const NodeContext &ctx)
{
    // Authentication first, then session. Both are rules that exist to be hard
    // to bypass, so they are checked before anything an attacker controls could
    // make the function return early for a friendlier reason — otherwise a
    // caller could probe which of its fields the node dislikes without ever
    // holding a key.
    if (!ctx.frame_authenticated)               return ArmRefusal::NotAuthenticated;
    if (!ctx.has_session)                       return ArmRefusal::NoSession;
    if (r.mode == Mode::Sweep && !ctx.is_bench_node)
                                                return ArmRefusal::SweepOffBench;
    if (ctx.battery_mv < kMinBatteryMv)         return ArmRefusal::BatteryTooLow;
    if (r.copies < kMinCopies || r.copies > kMaxCopies)
                                                return ArmRefusal::BadCopies;
    if (periodMattersFor(r.mode))
    {
        if (r.grid_period_ms == 0)              return ArmRefusal::NoGridPeriod;
        if (periodsAreCommensurate(r.grid_period_ms, ctx.rx_interval_ms))
                                                return ArmRefusal::CommensurateGrid;
    }
    return ArmRefusal::None;
}

// --- Restoring -------------------------------------------------------------
//
// What must be put back on EVERY exit path, including the timer's. The list is
// a struct rather than prose because the failure it prevents is silent and
// remote: a node left in Mode B against a hub that has forgotten the grid is a
// node that has stopped answering, and nothing about it looks broken.
struct SavedState {
    bool     valid{false};        // false = nothing was captured, restore is a no-op
    uint8_t  mode{0};             // the node's mode before the test
    uint8_t  slot{0};             // and its slot assignment
    bool     power_profile_production{true};
    bool     continuous_rx{false};
    uint32_t sym_timeout{0};
    bool     counter_enabled{true};
    bool     crypto_enabled{true};
    bool     deep_sleep_allowed{true};
};

// --- Distributions ---------------------------------------------------------
//
// Five order statistics and a count, per test-plan section 10.2. Not a full
// histogram: the point is the tail, and a mean would hide it.
struct Hist {
    int32_t  min{0};
    int32_t  p50{0};
    int32_t  p95{0};
    int32_t  p99{0};
    int32_t  max{0};
    uint32_t n{0};
};

// A bounded sample buffer. Fixed capacity because this runs on the node with no
// allocator in the measurement path; once full it keeps the EXTREMES and
// decimates the middle, since a tail statistic computed from samples that
// stopped arriving early is worse than one computed from fewer, spread samples.
static constexpr uint32_t kMaxSamples = 128;

struct Samples {
    int32_t  v[kMaxSamples]{};
    uint32_t n{0};          // how many slots are filled
    uint32_t offered{0};    // how many were ever added — Hist::n reports this

    void clear() { n = 0; offered = 0; }

    void add(int32_t x)
    {
        offered++;
        if (n < kMaxSamples) { v[n++] = x; return; }
        // Full. Drop one of the middle samples so the buffer keeps spanning the
        // whole run rather than only its beginning. Deterministic, so a test
        // can reason about it: replace the element at a rotating middle index.
        const uint32_t mid = kMaxSamples / 2 + (offered % (kMaxSamples / 4));
        v[mid] = x;
    }

    // Order statistics. Sorts a local copy — the caller's insertion order is not
    // meaningful and the buffer is small enough that this is cheaper than
    // keeping it sorted on every add, which happens in the measurement path.
    Hist summarise() const
    {
        Hist h;
        h.n = offered;
        if (n == 0) return h;

        int32_t s[kMaxSamples];
        for (uint32_t i = 0; i < n; ++i) s[i] = v[i];
        for (uint32_t i = 1; i < n; ++i)          // insertion sort; n <= 128
        {
            const int32_t key = s[i];
            uint32_t j = i;
            while (j > 0 && s[j - 1] > key) { s[j] = s[j - 1]; --j; }
            s[j] = key;
        }

        h.min = s[0];
        h.max = s[n - 1];
        h.p50 = s[percentileIndex(n, 50)];
        h.p95 = s[percentileIndex(n, 95)];
        h.p99 = s[percentileIndex(n, 99)];
        return h;
    }

    // Nearest-rank, clamped. With n below 100 the p99 slot is the maximum, and
    // that is the honest answer rather than an interpolation that invents a
    // value between two samples that were never observed.
    static constexpr uint32_t percentileIndex(uint32_t count, uint32_t pct)
    {
        if (count == 0) return 0;
        const uint32_t rank = (count * pct + 99) / 100;   // ceil
        const uint32_t idx  = (rank == 0) ? 0 : rank - 1;
        return idx >= count ? count - 1 : idx;
    }
};

// --- The funnel, as this mode reports it ----------------------------------
//
// MacFunnel.h already owns the counters and the rate arithmetic; this is only
// the extra one ModeTest can compute and the MAC cannot: gaps in the hub's own
// mark sequence. It is deliberately independent of MAC-1 being enabled, so a
// run with the counter off still knows what it missed.
struct SeqTracker {
    uint32_t first{0};
    uint32_t last{0};
    uint32_t received{0};
    uint32_t gaps{0};
    bool     started{false};

    void note(uint32_t seq)
    {
        if (!started) { first = seq; last = seq; received = 1; started = true; return; }
        if (seq > last + 1) gaps += (seq - last - 1);
        // A seq at or below `last` is a retransmit or a reorder, not a gap.
        if (seq > last) last = seq;
        received++;
    }

    // Marks the hub sent that never arrived. Derived, not counted, so it cannot
    // disagree with first/last.
    constexpr uint32_t expected() const
    {
        return started ? (last - first + 1) : 0;
    }
};

}  // namespace modetest
