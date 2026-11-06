#pragma once
// NodeLinkTiming.h -- one node's link timing on the hub (review HB10, split
// proposal 4). HUB-ONLY.
//
// Owns the hub's Mode B / Mode C placement for one node and the evidence it
// rests on (§4.6): the HubBelief, the phase-report and in-slot confirmations,
// the last placed mark and its shape, this node's own last uplink stamp (the
// Class A origin) and its mark->ack latency history. It decides single shot
// vs burst, places a frame on the node's mark or in its Class A window, and
// reaches the listener ONLY through LinkTimingHost (the grid, the node's
// reported mode, the operator's switches, MAC-0 send).
//
// Loop task only (docs/threading-model.md). The esp_timer bench streams place
// through the listener's send_placed_from_timer_, never through here.

#include <cstddef>
#include <cstdint>

#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/HubLinkSecurity.h"
#include "esphome/components/lora_client/SingleShotAckWait.h"
#include "esphome/components/lora_client/TimedGrid.h"
#include "esphome/components/lora_client/TimedModePolicy.h"

namespace esphome
{
  namespace lora_tracker
  {
    struct TxPolicy;

    // Where send_into_class_a_window put a frame, or why it did not. A node
    // that is not Class A, or one never heard, gets the burst: the hub does not
    // know where it listens. A Class A node whose RX1 and RX2 have BOTH closed
    // is asleep, and a burst aimed there is 1408 ms of air with nothing
    // listening. Numbered for the log line, not published to Home Assistant.
    enum class ClassAPlacement : uint8_t
    {
      Placed        = 0,  // one copy is queued on an open window
      NotClassA     = 1,  // this node opens no Class A window — burst
      NoUplinkStamp = 2,  // never heard an uplink from it — burst
      WindowsPast   = 3,  // Class A, both windows closed: it is ASLEEP
      NotSendable   = 4,  // no tracker or no payload — send nothing
      Dropped       = 5,  // aimed, but the transmit pool refused it (HB13)
    };

    // What a node's link timing may ask of its listener.
    class LinkTimingHost
    {
    public:
      virtual const char *link_name() const = 0;
      // A tracker to send through.
      virtual bool link_has_radio() const = 0;
      // The node was given the grid, and the tracker's grid is running.
      virtual bool link_grid_aligned() const = 0;
      virtual bool link_grid_started() const = 0;
      virtual uint32_t link_slot() const = 0;
      // The tracker's grid, for this node's slot.
      virtual int64_t link_next_t0(int64_t now_us) const = 0;
      virtual int64_t link_next_clear_t0(int64_t now_us, int copies, size_t len,
                                         bool expects_reply) const = 0;
      // Hand one frame to MAC-0.
      virtual bool link_send(const uint8_t *buf, size_t len, const TxPolicy &policy,
                             const mac2::FrameSeal &seal) = 0;
      // The operator's Timed Mode switch and optimistic single shot switch.
      virtual bool link_timed_mode_enabled() const = 0;
      virtual bool link_optimistic_single_shot() const = 0;
      // The resyncMaxS this hub published to the node (0 before a grid).
      virtual uint32_t link_resync_max_s() const = 0;
      // The node missed a check-in it was predicted to make (U-5).
      virtual bool link_beacon_overdue() const = 0;
      // The node's firmware version is known (from a decrypted beacon).
      virtual bool link_firmware_known() const = 0;
      // The node's last reported mode (NodeMode).
      virtual uint32_t link_node_mode() const = 0;
      // Mode C's ledger, stage 0: a frame was aimed at a window believed open.
      virtual void link_class_a_offered() = 0;

    protected:
      ~LinkTimingHost() = default;
    };

    class NodeLinkTiming
    {
    public:
      // The node transmits its uplink this long after its own mark (published
      // in GridSync as uloffsetus).
      static constexpr uint32_t kUplinkOffsetUs = 60000;

      explicit NodeLinkTiming(LinkTimingHost &host) : host_(host) {}
      NodeLinkTiming(const NodeLinkTiming &) = delete;
      NodeLinkTiming &operator=(const NodeLinkTiming &) = delete;

      // ---- placement ----
      // Place a frame on this node's next clear mark (single shot or burst by
      // §4.6), or hand it to MAC-0 as it is when the node has no grid.
      bool send_aligned(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal);
      bool send_aligned(const uint8_t *buf, size_t len, const TxPolicy &in_policy,
                        const mac2::FrameSeal &seal);
      // The mark a placed frame for this node would go out on (shared with
      // send_grid_sync, which must declare the round it is transmitted in).
      int64_t next_placement_t0(int64_t now_us, int copies, size_t len, bool expects_reply) const;
      // One copy in this node's next OPEN Class A window (RX1, else RX2).
      ClassAPlacement send_into_class_a_window(const uint8_t *buf, size_t len,
                                               const mac2::FrameSeal &seal)
      {
        return this->send_into_class_a_window_from(buf, len, seal, this->last_uplink_t0_us_);
      }
      // The same, off an explicit uplink T0 (an uplink not committed: the
      // keyless one that shows a node with a pending login is listening).
      ClassAPlacement send_into_class_a_window_from(const uint8_t *buf, size_t len,
                                                    const mac2::FrameSeal &seal, int64_t t0_uplink);
      // One copy on a Mode B node's own mark, recovered from its uplink's T0
      // (the node transmits at its mark + kUplinkOffsetUs). False: not placed
      // (no or a stale stamp, or the queue refused it).
      bool send_on_uplink_mark(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal,
                               int64_t t0_uplink);
      // The normal BURST with copy 0 on one of those marks, not before
      // `not_before_us` (the queue's drain): a Mode B node catches copy 0 in its
      // window, a Mode A node still has every copy (rollout-4: the LOGIN that
      // answers a REGISTER). False: not placed (no stamp, one older than
      // kUplinkMarkBurstMaxAgeUs, or the queue refused it).
      bool send_burst_on_uplink_mark(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal,
                                     int64_t t0_uplink, int64_t not_before_us);
      // The normal BURST with copy 0 at `phase_fire_us` + whole rounds, the first
      // such instant not before `not_before_us` (and the prepare lead); a phase of
      // 0 means "as soon as possible". Returns the fire instant (0: not sent). The
      // LOGIN phase sweep after a hub restart (HubSession::kLoginSweepOffsetsUs).
      int64_t send_burst_at_phase(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal,
                                  int64_t phase_fire_us, int64_t not_before_us);
      // How old an uplink stamp may be to place a burst from: a REGISTER is
      // re-sent every 60 s, and 75 s of +-20 ppm is +-1.5 ms of a 14 ms guard.
      static constexpr int64_t kUplinkMarkBurstMaxAgeUs = 75000000;

      // The fire instant of the first of the node's marks (uplink T0 minus the
      // uplink offset, plus whole rounds) that fires at or after `earliest_usable`.
      static int64_t uplink_mark_fire_(int64_t t0_uplink, int64_t earliest_usable);
      // `phase_us` + whole rounds, the first at or after `earliest_usable`.
      static int64_t next_at_phase_(int64_t phase_us, int64_t earliest_usable);

      // ---- evidence ----
      // This node's authenticated uplink: its T0 stamp and uncertainty, and
      // whether it landed in the node's slot.
      void commit_uplink_stamp(int64_t t0_us, uint32_t unc_us);
      void note_uplink_placement(int64_t t0_uplink_us);
      void note_phase_report(const ::PhaseReport *pr);
      // The belief as single shot judges it NOW (adds the time-dependent facts).
      timedmode::HubBelief belief_now() const;
      timedmode::TxPolicy tx_policy_now() const
      {
        return timedmode::txPolicyFor(this->belief_now(), timedgrid::kGuardUs,
                                      this->host_.link_resync_max_s());
      }
      // Rule 4: a single shot went unacked.
      void mark_single_shot_unacked() { this->belief_.single_shot_unacked = true; }

      // ---- what invalidates the evidence ----
      // The node restarted (its REGISTER): its grid phase is gone.
      void note_node_rebooted();
      // A new login challenge: counters, keys, maybe a reboot.
      void note_session_changed();
      // The hub no longer holds the grid the belief describes.
      void forget_grid();
      // The grid was (re)published: confidence starts at zero.
      void reset_belief();

      // ---- reads ----
      const timedmode::HubBelief &belief() const { return this->belief_; }
      bool     have_phase_report() const { return this->have_phase_report_; }
      int64_t  last_placed_t0_us() const { return this->last_placed_t0_us_; }
      bool     last_placed_single_shot() const { return this->last_placed_single_shot_; }
      bool     last_placed_optimistic() const { return this->last_placed_optimistic_; }
      int64_t  last_uplink_t0_us() const { return this->last_uplink_t0_us_; }
      uint32_t last_uplink_unc_us() const { return this->last_uplink_unc_us_; }
      bool     have_rx_busy_skips() const { return this->have_rx_busy_skips_; }
      uint32_t node_rx_busy_skips() const { return this->node_rx_busy_skips_; }
      singleshotwait::AckLatency &ack_latency() { return this->ack_latency_; }

      // ---- test seams ----
      timedmode::HubBelief &belief_for_test() { return this->belief_; }
      bool have_in_slot_confirm_for_test() const { return this->have_in_slot_confirm_; }
      void set_in_slot_confirm_for_test(bool v) { this->have_in_slot_confirm_ = v; }
      void set_last_uplink_t0_for_test(int64_t t0_us) { this->last_uplink_t0_us_ = t0_us; }
      void set_last_placed_t0_for_test(int64_t t0_us) { this->last_placed_t0_us_ = t0_us; }

    private:
      LinkTimingHost &host_;
      // §4.6's hub half: every field maintained from a real event, nothing a
      // claim the node makes about itself.
      timedmode::HubBelief belief_{};
      // When this node's uplink was last OBSERVED in its slot. An explicit
      // flag rather than a 0 sentinel: esp_timer starts near zero (AT zero
      // under the host harness), so 0 cannot mean "never".
      bool     have_in_slot_confirm_{false};
      int64_t  last_in_slot_us_{0};
      // The shape of the LAST frame send_aligned placed, whoever placed it --
      // not the tracked command's (the delivery service's sent_single_shot), which its
      // senders copy from here themselves.
      bool     last_placed_single_shot_{false};
      // Whether that frame was a single shot ONLY because the optimistic switch
      // bypassed ConfirmationStale.
      bool     last_placed_optimistic_{false};
      // THIS node's mark->ack delays for single placed copies
      // (SingleShotAckWait.h). One per node, so 32 nodes never share a latency.
      singleshotwait::AckLatency ack_latency_;
      // The mark the last placed downlink was aimed at, so a second command
      // inside the same round goes to the FOLLOWING mark. 0 = nothing placed.
      int64_t  last_placed_t0_us_{0};
      // When the node's last phase report arrived (its freshness is the age
      // of that beacon); the flag distinguishes "never" from "at boot".
      int64_t  last_phase_report_us_{0};
      bool     have_phase_report_{false};
      // U-2: windows the node meant to open and did not (radio busy at the
      // mark), from its PhaseReport; the flag says a report has arrived.
      uint32_t node_rx_busy_skips_{0};
      bool     have_rx_busy_skips_{false};
      // C2: when THIS node's most recent authenticated uplink left the air
      // (T0, the SFD end), and how uncertain that stamp is. 0 = no usable
      // origin: send_into_class_a_window answers NoUplinkStamp.
      int64_t  last_uplink_t0_us_{0};
      uint32_t last_uplink_unc_us_{0};
    };

  }  // namespace lora_tracker
}  // namespace esphome
