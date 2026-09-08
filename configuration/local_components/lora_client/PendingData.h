#pragma once

#include <stdint.h>

#include "TimedGrid.h"

// ---------------------------------------------------------------------------
// The pending-data bitmap (implementation-plan.md section 4.4, Tier 3).
//
// A beacon carries one bit per slot. A node whose bit is CLEAR knows the hub
// has nothing for it and can skip its private window until the next beacon —
// which is the whole point, because an armed window is ~29 ms of receive at
// ~11 mA against a ~1.2 mA average, and most windows on most nodes are empty.
//
// Everything here is about the two ways that idea goes wrong.
//
// ABSENT IS NOT EMPTY. A zeroed proto3 field is indistinguishable from a
// deliberate "nothing for anyone", so reading a missing bitmap as all-clear
// would put every node in the fleet to sleep on the first beacon that omitted
// it — or on the first firmware that did not know the field. The mask carries
// its own validity, and an invalid mask means LISTEN. Same principle as
// MacSublayers::Config: the zeroed value is the safe one, not the permissive
// one.
//
// A SKIP MUST EXPIRE. A node that skips until "the next beacon" and then misses
// that beacon would skip forever. Beacons are every 233 rounds by default —
// ~5.8 minutes — so a single lost beacon would otherwise cost a node nearly six
// minutes of deafness, and a hub that stopped beaconing would cost it all of
// them. The permission is therefore bounded by round number, not by "until
// something else happens", and it expires on the round the next beacon is due
// whether or not that beacon arrives.
//
// Dependency-free apart from TimedGrid.h, which supplies the slot count the
// bitmap is sized against.
// ---------------------------------------------------------------------------

namespace pending {

// One bit per slot, so the grid's slot count and the mask width must agree.
static_assert(timedgrid::kSlotCount <= 32,
              "the pending-data bitmap is 32 bits; a wider grid needs a wider field");

struct Mask
{
    uint32_t bits{0};
    // False until a beacon actually carried one. A zeroed Mask therefore means
    // "unknown", and unknown means listen.
    bool     valid{false};
    // The round the carrying beacon belonged to, and how often beacons come.
    // Together these say when this permission stops being trustworthy.
    uint32_t beacon_round{0};
    uint32_t beacon_every_rounds{0};
};

constexpr bool hasDataFor(const Mask &m, uint32_t slot)
{
    if (slot >= timedgrid::kSlotCount) return true;   // out of range: listen
    return (m.bits & (1u << slot)) != 0;
}

// The round at which this mask stops being believed.
//
// One beacon interval after the beacon that carried it. Not two: a node that
// kept trusting a stale mask across a missed beacon would be trusting the hub's
// state from over eleven minutes ago.
constexpr uint32_t expiresAtRound(const Mask &m)
{
    return (m.beacon_every_rounds == 0)
         ? m.beacon_round                       // no cadence known: expires now
         : m.beacon_round + m.beacon_every_rounds;
}

// Should the node arm its receive window for `round`?
//
// The answer is YES unless every condition for skipping is met, and it is
// written that way deliberately: each early return is a reason to listen, so a
// field this function does not understand, or a state it has not been told
// about, produces a listening node rather than a deaf one.
constexpr bool shouldArmWindow(const Mask &m, uint32_t slot, uint32_t round)
{
    if (!m.valid)                    return true;   // never had a bitmap
    if (slot >= timedgrid::kSlotCount) return true; // unassigned slot
    if (round >= expiresAtRound(m))  return true;   // stale, or beacon missed
    if (round < m.beacon_round)      return true;   // clock moved backwards
    return hasDataFor(m, slot);
}

// How many consecutive windows a node may skip on one beacon. Reported rather
// than assumed, because it is the number that decides the battery saving and
// it is entirely the hub's beacon cadence — the node has no say in it.
constexpr uint32_t maxSkippableRounds(const Mask &m, uint32_t slot)
{
    if (!m.valid || hasDataFor(m, slot)) return 0;
    return m.beacon_every_rounds;
}

// --- The hub's side --------------------------------------------------------

// Set or clear a slot's bit. Separate from Mask so the hub can build one
// incrementally as it walks its transmit queue.
constexpr uint32_t withSlot(uint32_t bits, uint32_t slot, bool has_data)
{
    if (slot >= timedgrid::kSlotCount) return bits;
    return has_data ? (bits | (1u << slot)) : (bits & ~(1u << slot));
}

// A hub that cannot enumerate its pending traffic must publish a mask that
// makes every node listen, not an empty one. This is that mask.
constexpr uint32_t allListening()
{
    return (timedgrid::kSlotCount >= 32)
         ? 0xFFFFFFFFu
         : ((1u << timedgrid::kSlotCount) - 1u);
}

}  // namespace pending
