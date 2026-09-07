#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// MacSublayers — MAC-1 (frame counter) and MAC-2 (security) as independently
// switchable sublayers. See configuration/docs/mac-layer.md section 4.
//
// WHY SWITCHABLE. Run MAC-0 and record the KPIs; switch on MAC-1 and re-run the
// identical grid, and the change in yield is the replay window while the change
// in turnaround is the counter check; switch on MAC-2 and the further delta is
// AEAD. Each sublayer's cost becomes a MEASURED NUMBER instead of an estimate.
// That matters concretely: the servable-slot geometry turns on a node
// turnaround that has never been measured, and AES-GCM over up to 152 bytes
// sits inside it.
//
// THE SCOPE IS DELIBERATELY NARROW, AND THIS IS THE SAFETY ARGUMENT.
//
// The switches apply to MAC CONTROL FRAMES ONLY. Application traffic — every
// cover operation, schedule, login and config — always passes through MAC-1 and
// MAC-2 regardless of what is set here. A switch that could disable the replay
// window or authentication for real commands would be a remote-unlock hole
// wearing a measurement's clothes, and no attribution benefit is worth that.
//
// So what these buy is the ability to measure the MAC on its own, on traffic
// that carries no authority: a MacControl frame cannot move a blind, change a
// schedule, or alter configuration no matter who sends it.
//
// ARMING IS AUTHENTICATED EVEN WHEN THE TRAFFIC IS NOT.
//
//   - a node with NO SESSION must refuse to arm. Otherwise the unauthenticated
//     bootstrap window becomes a way to hold 32 nodes in a test configuration.
//   - the arming frame itself must have arrived AUTHENTICATED (through the AEAD
//     path). A plaintext frame asking to turn authentication off answers its
//     own question.
//   - arming is time-bounded and the node owns the deadline, so a hub that
//     disappears mid-test cannot leave a node parked with a sublayer off.
//
// Dependency-free, so the decision table is verified on the host.
// ---------------------------------------------------------------------------

namespace macsublayers
{

// Node-owned cap on how long a sublayer may stay disabled. The hub may ask for
// less; it may not ask for more, and it may not ask for "forever".
static constexpr uint32_t kMaxDisableS     = 900;   // 15 minutes
static constexpr uint32_t kDefaultDisableS = 300;

constexpr uint32_t disableDurationS(uint32_t requested)
{
    if (requested == 0)              return kDefaultDisableS;
    if (requested > kMaxDisableS)    return kMaxDisableS;
    return requested;
}

struct Config
{
    // BOTH DEFAULT ON. A zeroed Config — proto3 defaults, a cold boot, a
    // corrupted NVS blob — is the SAFE configuration, not the permissive one.
    bool counter_enabled{true};   // MAC-1
    bool crypto_enabled{true};    // MAC-2
};

// Why an arming request was refused. The caller reports these, because a
// silently ignored test command is indistinguishable from a lost one.
enum class ArmRefusal : uint8_t {
    None = 0,
    NoSession,        // nothing to authenticate against
    NotAuthenticated, // the request itself arrived in plaintext
    NotBenchNode,     // deliberately-degrading modes are bench-only
};

// `is_bench_node` gates only the configurations that deliberately break
// reception; a plain MAC-0 measurement run is legitimate in the field.
constexpr ArmRefusal armRefusal(bool has_session, bool request_was_authenticated,
                                bool is_bench_node, bool wants_degrading_mode)
{
    if (!has_session)                                return ArmRefusal::NoSession;
    if (!request_was_authenticated)                  return ArmRefusal::NotAuthenticated;
    if (wants_degrading_mode && !is_bench_node)      return ArmRefusal::NotBenchNode;
    return ArmRefusal::None;
}

constexpr bool mayArm(bool has_session, bool request_was_authenticated,
                      bool is_bench_node, bool wants_degrading_mode)
{
    return armRefusal(has_session, request_was_authenticated, is_bench_node,
                      wants_degrading_mode) == ArmRefusal::None;
}

// The decision the RX path asks, per frame.
//
// `is_mac_control` is the whole safety hinge: when it is false the sublayers
// are unconditionally on, whatever Config says.
constexpr bool counterCheckRequired(const Config &cfg, bool is_mac_control)
{
    return !is_mac_control || cfg.counter_enabled;
}

constexpr bool cryptoRequired(const Config &cfg, bool is_mac_control)
{
    return !is_mac_control || cfg.crypto_enabled;
}

}  // namespace macsublayers
