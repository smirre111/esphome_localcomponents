#pragma once
// hub_grid.h -- the hub's timed grid: its anchor and the arithmetic of rounds,
// slots and beacon marks (review HB10, tracker split part 4). HUB-ONLY,
// dependency-free apart from TimedGrid.h's constants, so it is host-testable.
//
// The anchor is set once (start) and never moved: every node's phase, every
// declared fire stamp and every beacon round is measured from it. Clearance
// (which mark is free of the hub's own reservations) is the transmit
// scheduler's question, not the grid's.

#include <cstdint>

#include "esphome/components/lora_client/TimedGrid.h"

namespace esphome
{
  namespace lora_tracker
  {
    class HubGrid
    {
    public:
      // Anchor the grid at `now_us`. False (and nothing moves) when it already is.
      bool start(int64_t now_us)
      {
        if (this->started_)
          return false;   // set once, never moved
        this->anchor_us_ = now_us;
        this->started_   = true;
        return true;
      }
      bool    started() const   { return this->started_; }
      int64_t anchor_us() const { return this->anchor_us_; }

      // The round in which `slot`'s mark at `t0_us` falls (0 before the anchor
      // or without a grid).
      uint32_t round_for_slot_t0(uint8_t slot, int64_t t0_us) const
      {
        if (!this->started_)
          return 0;
        const int64_t rel = t0_us - this->anchor_us_
                          - (int64_t) (slot % timedgrid::kSlotCount)
                              * (int64_t) timedgrid::kSlotPitchUs;
        if (rel < 0)
          return 0;
        return (uint32_t) (rel / (int64_t) timedgrid::kRoundUs);
      }

      // This T0 as the grid describes it: round since the anchor and the offset
      // into that round. False without a grid, or before the anchor.
      bool fire_stamp_for(int64_t t0_us, uint32_t *round, uint32_t *offset_us) const
      {
        if (!this->started_)
          return false;
        const int64_t rel = t0_us - this->anchor_us_;
        if (rel < 0)
          return false;
        if (round != nullptr)
          *round = (uint32_t) (rel / (int64_t) timedgrid::kRoundUs);
        if (offset_us != nullptr)
          *offset_us = (uint32_t) (rel % (int64_t) timedgrid::kRoundUs);
        return true;
      }

      uint32_t beacon_round_for_t0(int64_t t0_us) const
      {
        return this->round_for_slot_t0((uint8_t) timedgrid::kBeaconSlotIndex, t0_us);
      }

      // The next beacon mark at or after `now_us`; 0 without a grid or beacons.
      int64_t next_beacon_t0(int64_t now_us) const
      {
        if (!this->started_ || timedgrid::kBeaconEveryRounds == 0)
          return 0;
        // The beacon slot's marks come every round; only every Nth is a beacon.
        const int64_t stride = (int64_t) timedgrid::kRoundUs
                             * (int64_t) timedgrid::kBeaconEveryRounds;
        const int64_t base   = this->anchor_us_
                             + (int64_t) timedgrid::kBeaconSlotIndex
                                 * (int64_t) timedgrid::kSlotPitchUs;
        if (now_us <= base)
          return base;
        return base + ((now_us - base + stride - 1) / stride) * stride;
      }

      // `slot`'s next mark at or after `now_us`; `now_us` itself without a grid
      // (send now, do not invent an instant).
      int64_t next_t0_for_slot(uint8_t slot, int64_t now_us) const
      {
        if (!this->started_)
          return now_us;

        const int64_t pitch = (int64_t) timedgrid::kSlotPitchUs;
        const int64_t round = (int64_t) timedgrid::kRoundUs;
        const int64_t base  = this->anchor_us_
                            + (int64_t) (slot % timedgrid::kSlotCount) * pitch;

        // Rounds elapsed since that slot's first T0, rounded UP so the answer is
        // never in the past. Integer division truncates towards zero, which for a
        // negative delta would round the wrong way — hence the explicit branch
        // rather than a (d + round - 1) / round that only works for d >= 0.
        const int64_t delta = now_us - base;
        if (delta <= 0)
          return base;
        const int64_t rounds = (delta + round - 1) / round;
        return base + rounds * round;
      }

    private:
      int64_t anchor_us_{0};
      bool    started_{false};
    };

  }  // namespace lora_tracker
}  // namespace esphome
