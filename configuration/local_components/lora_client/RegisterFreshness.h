#pragma once
// RegisterFreshness.h -- the hub's replay check for REGISTER (review HB14).
// HUB-ONLY, dependency-free (host-tested through the listener).
//
// REGISTER bypasses the session's replay window (an unprovisioned node has no
// session), and its FCNT restarts at every boot, so it cannot be the replay key.
// The node numbers its REGISTERs instead: ClientRegister.register_seq is
// strictly increasing per node and persisted before use, and the K_AUTH tag
// covers it. The hub keeps the highest value it has seen per MAC (RAM, current
// boot) and refuses any value not above it. 0 is a node that predates the
// field: accepted (the 5 s rate limit still applies) until that node has sent
// a numbered REGISTER, after which an unnumbered one can only be an old capture.

#include <cstdint>

namespace regfresh
{
  class Highest
  {
  public:
    // True when `seq` may be taken; records it. Call only for a REGISTER whose
    // tag verified, and before the rate limit (a genuine frame the rate limit
    // drops still raises the bar for its own replay).
    bool admit(uint32_t seq)
    {
      if (seq == 0)
        return this->highest_ == 0;
      if (seq <= this->highest_)
        return false;
      this->highest_ = seq;
      return true;
    }
    uint32_t highest() const { return this->highest_; }

  private:
    uint32_t highest_{0};
  };
}  // namespace regfresh
