#pragma once
// FrameCanon.h -- is the frame's ON-AIR LENGTH trustworthy as a timing input?
//
// Dependency-free policy header (no protobuf-c, no ESP-IDF): the caller hands
// in three integers it already has, this decides. Vendored byte-identical into
// the hub (local_components/lora_client/FrameCanon.h), gated by a drift test.
//
// THE PROBLEM
//   Both ends recover T0 (the SFD end, LoraTiming.h) from RxDone as
//       T0 = t_rxdone - n_sym(len) * T_sym
//   so the on-air byte length is a TIMING input. The CMAC authenticates the
//   decoded VALUES and ctLen, but not the protobuf BYTE ENCODING. protobuf
//   parsers are lenient, so a genuine, correctly-tagged frame can be
//   re-presented with the same decoded content in a different number of bytes:
//     * a duplicated field        (last value wins, the earlier copy is dead
//                                  weight on the air, re-encode is SHORTER)
//     * fields in a different order / non-minimal varints (re-encode differs)
//     * an appended field the parser does not know (kept as an unknown field
//       and, in protobuf-c, COUNTED by get_packed_size, so a plain length
//       comparison would not see it)
//   Each shifts n_sym(len) and so T0, by whole symbols (~1 ms each at SF7-ish
//   settings), on content that still verifies. mac1freshness::CurrentBurstCopy
//   is the path that matters: a replay of the burst copy being accepted is
//   classed as fresh and feeds the phase/anchor bookkeeping.
//
// THE CHECK (receiver, after unpack, on the OUTER message and its header)
//   canonical  <=>  packed_size(decoded) == rx_len   AND   no unknown fields
//   A peer that encodes with protobuf-c (both ends do) always produces a
//   canonical frame, so a same-version link NEVER trips either clause.
//
// THE DECISION: DEMOTE, DO NOT DROP.
//   A non-canonical frame is NOT rejected. It is processed exactly as before
//   (its content is authenticated by the CMAC when encrypted; unauthenticated
//   frames are gated elsewhere), but its length is declared untrustworthy, so
//   it is NOT used for anything that derives a T0 from it: phase-sample commit,
//   anchor re-solve, GridSync/GridBeacon/ModeTest timing, drift-rate marks,
//   last_addressed bookkeeping (node), and the Class-A uplink stamp (hub).
//   Why not drop:
//     1. Forward compatibility is a standing rollout rule (node first, old
//        parser tolerates new fields; auto-mode-plan.md P0). A newer peer's
//        frame carrying a field this build does not know is LEGITIMATE and its
//        command must still run. Dropping it would turn every proto bump into
//        a flag day.
//     2. Dropping buys nothing security-wise: a padded replay of an authentic
//        frame carries authentic content and is deduplicated by msgId/replay
//        window like any replay. The only thing the padding can abuse is the
//        timing, and demotion removes exactly that.
//   Why unknown fields demote even when the lengths match: protobuf-c counts
//   them in get_packed_size, so an attacker-appended unknown field makes the
//   lengths agree while still skewing T0. They sit outside the CMAC. The price
//   is that a NEWER peer's frames lose timing use until this side is updated
//   (the node falls back to the next canonical frame; the hub's Class-A reply
//   falls back to the burst) -- degraded, never wrong.
//   Precedence: a length mismatch is reported ahead of unknown fields, because
//   it is the stronger signal (a forward-compat extra field matches in length).
#include <cstdint>

namespace framecanon
{

enum class Verdict : uint8_t
{
    Canonical,        // re-encodes to the received length, nothing unknown
    LengthMismatch,   // re-encode differs: padding, duplicate/reordered fields
    UnknownFields,    // length agrees but the message carries unknown fields
};

struct Inputs
{
    uint32_t rx_len;        // bytes the radio delivered
    uint32_t repacked_len;  // get_packed_size() of the decoded outer message
    uint32_t n_unknown;     // unknown fields on the outer message + its header
};

inline Verdict classify(const Inputs &in)
{
    if (in.repacked_len != in.rx_len)
        return Verdict::LengthMismatch;
    if (in.n_unknown != 0)
        return Verdict::UnknownFields;
    return Verdict::Canonical;
}

// May this frame's length be used to recover a T0?
inline bool timingTrusted(Verdict v) { return v == Verdict::Canonical; }

}  // namespace framecanon
