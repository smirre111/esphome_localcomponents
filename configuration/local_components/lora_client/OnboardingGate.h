#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// OnboardingGate — one node's login-to-settled window at a time.
//
// THE PROBLEM. The hub has ONE downlink queue and it drains at ~1.85 s per frame
// (a 17-copy burst plus its response window). Bringing a node up costs eight or
// so frames: LoginMsg (asking for a REGISTER), the login again, ClientConfig,
// CoverConfig, TimeSync, GridSync, ScheduleConfig, and the retries. That is
// 15+ s of airtime — and the only thing keeping two nodes apart was a 3 s
// stagger between their FIRST login. Measured 2026-09-26 with two nodes after a
// hub restart: the handshakes interleaved, 16 bursts went out back to back for
// 30 s, one node's login sat 11 s in the queue, and every ScheduleConfig timed
// out while the node was acknowledging it.
//
// THE RULE. A node may START a login challenge only while it holds the gate.
// It holds it until it is SETTLED — session confirmed, the pushes that follow
// confirmation aired and acknowledged — or until it demonstrably is not going to
// be: it never answered, or it has held the gate too long. Waiters are served in
// the order they asked, so 32 nodes do not starve the ones that lost the first
// race.
//
// WHAT IT IS NOT. It is not a protocol change and adds no frame type: it orders
// when the HUB begins something it was always going to do. A node that answers
// sees exactly the exchange it saw before, one node at a time.
//
// IDENTITY is a pointer, so no assignment order or numbering has to agree
// between the listeners. Time is a number of milliseconds the caller counts
// (its poll timer's period), so the policy needs no clock and a test needs none.
//
// Dependency-free.
// ---------------------------------------------------------------------------

namespace onboarding
{

// How often the holder re-assesses. Also the unit every age below is counted in.
static constexpr uint32_t kPollMs = 500;

// After the session is confirmed the hub still has to queue its follow-up pushes:
// TimeSync at +0.75 s, GridSync at +1.25 s, ScheduleConfig at +2 s. The gate
// cannot be released before the last of them has even been handed to the queue.
static constexpr uint32_t kSettleAfterConfirmMs = 3000;

// A node that has not answered anything this long after its login went out is
// not coming to the phone. Two login bursts and their response windows fit in
// this; letting it hold the gate longer only makes the nodes that ARE there wait.
static constexpr uint32_t kSilentAfterMs = 15000;

// The ceiling on any one hold. Every retry ladder in the listener ends well
// inside it; it exists so a state nobody anticipated cannot park the gate.
static constexpr uint32_t kMaxHoldMs = 60000;

static constexpr size_t kMaxWaiters = 32;

enum class Verdict : uint8_t
{
  Keep       = 0,   // still onboarding, hold the gate
  Settled    = 1,   // session up and the follow-up pushes are done
  NodeSilent = 2,   // never answered: let the others go first
  Capped     = 3,   // held too long: something unanticipated, release regardless
};

struct HoldState
{
  uint32_t age_ms{0};                // since the gate was granted
  bool     confirmed{false};         // the node proved it holds the session key
  uint32_t since_confirm_ms{0};
  bool     node_heard{false};        // any frame from the node since the grant
  int64_t  tx_drain_us{0};           // the hub's own queue, see LORATracker::txDrainUs
  bool     schedule_outstanding{false};   // a ScheduleConfig sent, not yet acked
};

inline Verdict assess(const HoldState &s)
{
  // The cap is checked first and unconditionally: it is the one clause that does
  // not depend on any other state being right.
  if (s.age_ms >= kMaxHoldMs)
    return Verdict::Capped;

  if (s.confirmed)
  {
    if (s.since_confirm_ms >= kSettleAfterConfirmMs &&
        s.tx_drain_us <= 0 && !s.schedule_outstanding)
      return Verdict::Settled;
    return Verdict::Keep;
  }

  // Not confirmed. Silence only counts once the hub's own frames have left the
  // radio: a login still queued behind another node's traffic has not been
  // "unanswered", it has not been asked.
  if (!s.node_heard && s.tx_drain_us <= 0 && s.age_ms >= kSilentAfterMs)
    return Verdict::NodeSilent;
  return Verdict::Keep;
}

// The gate itself: one holder, and a FIFO of those waiting.
class Gate
{
 public:
  // Grants the gate to `who` if it is free and `who` is first in line (or nobody
  // is). Idempotent for the holder. A denied caller is remembered, in order, so
  // asking again every poll is exactly what queues it.
  //
  // A waiter that stopped asking must not block a FREE gate for ever. A live
  // front-of-line takes a free gate on its next poll, and in that one poll at
  // most every other waiter asks once, so more denials than that can only mean
  // the front is gone (its timer was cancelled, its node given up on). It is then
  // dropped and the line moves. Counted in asks, not milliseconds, so it needs no
  // clock.
  bool tryAcquire(const void *who)
  {
    if (who == nullptr)
      return false;
    if (holder_ == who)
      return true;
    if (holder_ != nullptr)
    {
      enqueue(who);
      return false;
    }
    if (count_ > 0 && waiting_[0] != who)
    {
      if (++front_denials_ <= 2 * count_ + 2)
      {
        enqueue(who);
        return false;
      }
      popFront();                        // it never came back for a free gate
      front_denials_ = 0;
      if (count_ > 0 && waiting_[0] != who)
      {
        enqueue(who);
        return false;
      }
    }
    forget(who);
    front_denials_ = 0;
    holder_        = who;
    return true;
  }

  // Frees the gate if `who` holds it, and forgets `who` if it was only waiting.
  // Anyone else's release is ignored: a stale timer must not free another node's
  // hold.
  void release(const void *who)
  {
    if (holder_ == who)
      holder_ = nullptr;
    forget(who);
  }

  bool         held() const     { return holder_ != nullptr; }
  const void  *holder() const   { return holder_; }
  size_t       waiting() const  { return count_; }
  bool         isHolder(const void *who) const { return who != nullptr && holder_ == who; }
  void         reset()          { holder_ = nullptr; count_ = 0; front_denials_ = 0; }

 private:
  void forget(const void *who)
  {
    for (size_t i = 0; i < count_; ++i)
      if (waiting_[i] == who)
      {
        remove(i);
        return;
      }
  }
  void enqueue(const void *who)
  {
    for (size_t i = 0; i < count_; ++i)
      if (waiting_[i] == who)
        return;                         // already in line: keep its place
    if (count_ < kMaxWaiters)
      waiting_[count_++] = who;
    // A full line drops the newcomer; it asks again next poll, and 32 is the
    // whole fleet, so this cannot happen with distinct listeners.
  }
  void popFront() { remove(0); }
  void remove(size_t i)
  {
    for (size_t j = i + 1; j < count_; ++j)
      waiting_[j - 1] = waiting_[j];
    --count_;
  }

  const void *holder_{nullptr};
  const void *waiting_[kMaxWaiters]{};
  size_t      count_{0};
  size_t      front_denials_{0};
};

}  // namespace onboarding
