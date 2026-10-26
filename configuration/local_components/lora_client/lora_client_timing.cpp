// lora_client_timing.cpp -- the listener's per-node link timing: placement on
// the node's grid mark (send_aligned_, nextPlacementT0_), Class A replies, the
// hub's belief about the node (PhaseReport, in-slot uplinks, hubBeliefNow_)
// and the grid publication to the node (GridSync, GridDemote, regrid after a
// reboot, Timed Mode). Stage 5 split of lora_client.cpp, moved verbatim. Loop
// task only (docs/threading-model.md).
#include "lora_client_internal.h"

namespace esphome
{
  namespace lora_tracker
  {
    // Beacon placement (implementation-plan.md 4.4) now lives in TimedGrid.h,
    // with the geometry it belongs to: the tracker TRANSMITS the beacon and
    // this file ANNOUNCES it, and two copies is how those two come to disagree.
    using timedgrid::kBeaconSlotIndex;
    using timedgrid::kBeaconEveryRounds;


    // ---------------------------------------------------------------------------
    // F-4: Command ACK / retransmit state machine
    // ---------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // B1: place a downlink on the grid.
    //
    // Deferring means the buffer must be OWNED — every caller frees its packed
    // frame the moment this returns, so capturing the pointer would hand the
    // scheduler freed memory a second later.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // B3: publish the grid to this node, or withdraw it.
    //
    // The frame declares its OWN grid position so the node can solve for a
    // local anchor — see GridState.h. A publish is PLACED so that declaration is
    // true, and still a full burst, because a node being told about the grid is
    // by definition not yet on it. A withdrawal is an unplaced burst.
    // -----------------------------------------------------------------------
    void LORAListener::send_grid_sync(bool enable)
    {
      if (!enable)
      {
        this->withdraw_grid_sync_();
        return;
      }
      // Publishing a grid needs the key it carries to be encrypted, so it waits
      // for the session (see relogin_pending_); confirm_session_ publishes it
      // again if timed mode is on. WITHDRAWING one is not deferred: it is the safe
      // direction and carries no key.
      if (this->relogin_pending_)
      {
        ESP_LOGD(TAG, "[%s] GridSync deferred — session being rebuilt", this->get_name().c_str());
        return;
      }
      // The publish is the Mac channel's request: re-published until the node's
      // MAC ACK names one of its msgids (the set), a re-publish is REBUILT, and
      // giving up only logs (the node stays in Mode A, the session is fine).
      DeliveryRequest req;
      req.what        = "GridSync";
      req.resend_verb = "re-publishing";
      req.policy      = deliverypolicy::kGridSyncPolicy;
      req.encode      = [this](bool retry, uint32_t *, const DeliveryRequest::FrameFn &frame) {
        return this->encode_grid_sync_(retry, frame);
      };
      req.place        = [this](const MacFrame &f, const DeliveryAttempt &) { this->place_grid_sync_(f); };
      req.on_delivered = [this](const DeliveryConfirm &c) { return this->on_grid_sync_acked_(c.log_id); };
      req.on_fail     = [this]() { this->on_grid_sync_failed_(); };
      this->deliver_(CH_MAC, std::move(req));
    }

    // The mark a publish will be placed on, chosen BEFORE the frame is built,
    // because the frame declares it. 0 = not on a running grid (unplaced).
    //
    // The length is a BOUND rather than the real figure, and it has to be:
    // gs.txround is computed from planned_t0, so the mark must be chosen before
    // the frame exists to be measured. Over-estimating is the safe direction — it
    // only makes the clearance search stricter, never places a frame into
    // occupied air. Observed GridSync frames run 93-95 B (94 B with the fleet
    // key); 128 is comfortably above that and far under the 256 B buffer. The
    // copy count matches what is actually requested in tx_grid_sync_.
    int64_t LORAListener::grid_sync_planned_t0_()
    {
      static constexpr size_t kGridSyncLenBound = 128;
      return (this->grid_aligned_ && this->parent_->gridStarted())
                 ? this->nextPlacementT0_(esp_timer_get_time(),
                                          this->parent_->defaultBurstCopies(),
                                          kGridSyncLenBound,
                                          /*expects_reply=*/false)
                 : 0;
    }

    // The fields of a PUBLISH (enable = true), declaring the frame's own position.
    void LORAListener::fill_grid_sync_(::GridSync *gs_out, int64_t planned_t0)
    {
      GridSync &gs = *gs_out;
      gs.enable = true;
      gs.slotindex        = this->grid_slot_;
      gs.slotcount        = timedgrid::kSlotCount;
      gs.roundus          = timedgrid::kRoundUs;
      gs.pitchus          = timedgrid::kSlotPitchUs;
      gs.beaconslotindex  = kBeaconSlotIndex;
      gs.beaconeveryrounds = kBeaconEveryRounds;
      gs.symtimeout       = timedgrid::kSymbolTimeoutSymbols;
      gs.resyncmaxs       = timedgrid::maxResyncIntervalS(20) / 2;   // half the
                                                                     // +/-20 ppm
                                                                     // ceiling
      // Remember what we published: it is the bound txPolicyFor judges the
      // age of the node's phase report against.
      this->published_resync_max_s_ = gs.resyncmaxs;
      gs.uloffsetus       = LORAListener::kUplinkOffsetUs;

      // This frame's own position. It is what the node anchors on, so it must
      // describe where the frame will ACTUALLY be transmitted. That was an
      // assertion nothing enforced: send_grid_sync called parent_->send()
      // directly, so the frame left whenever the queue drained and declared a
      // slot it did not occupy. It is now PLACED (tx_grid_sync_), so copy 0
      // really is at this slot's mark.
      //
      // THE ROUND THIS FRAME ACTUALLY GOES OUT IN. The anchor alone would not
      // need it (marks repeat every kRoundUs), but the node's round COUNTER,
      // which the beacon cadence phases off, does: with a wrong round the hub
      // and the node disagree about which round is a beacon round. The frame
      // is placed on planned_t0, so its round is known here.
      gs.txround = this->parent_->roundForSlotT0(this->grid_slot_, planned_t0);
      gs.txslot  = this->grid_slot_;

      // The pending-data bitmap (section 4.4): ALL LISTENING, this node's bit
      // included — the same promise the beacon makes, for the same reason.
      //
      // It used to clear this node's bit whenever nothing was queued for it at
      // the moment of publication. An empty bit is a licence to skip the
      // private window for a whole beacon interval (~5.8 min), and "nothing
      // queued right now" is not "nothing for 5.8 minutes": Home Assistant can
      // produce a command at any instant, and a ModeTest sends a mark every
      // round from a timer that queues nothing in advance. Measured 2026-09-14
      // on node 2 (Mode B production 900 s): enabling timed mode published a
      // cleared bit, the node promoted and then opened only BEACON windows —
      // 1611 s to ~2067 s, two beacon intervals, not one mark heard. Once it
      // listened, it caught 168 of 168.
      gs.pendingmask      = pending::allListening();
      gs.pendingmaskvalid = true;

      // Section 4.4's fleet key, and the one condition on carrying it.
      //
      // ONLY WHEN THIS FRAME WILL BE ENCRYPTED. v1_frame_
      // encrypts when a session exists and packs plaintext otherwise, so
      // assigning the key unconditionally would put it on the air in the
      // clear on exactly the path where no session had been established yet
      // — which is every bootstrap, and would make the beacon's MAC
      // decorative for the whole fleet. This is the §11a lesson in a new
      // place: a rule is only shipped when the caller satisfies it.
      //
      // A node that gets a keyless GridSync simply holds no key and treats
      // beacons the way it did before this existed: bounded re-anchor, no
      // mask. The hub re-publishes on the next grid change.
      if (this->session_confirmed_ && this->parent_ != nullptr &&
          this->parent_->netKeyId() != 0)
      {
        gs.netkey.data = const_cast<uint8_t *>(this->parent_->netKey());
        gs.netkey.len  = framecrypto::kNetKeyBytes;
        gs.netkeyid    = this->parent_->netKeyId();
      }
    }

    // ONE attempt at publishing the grid, built from the grid's position NOW (a
    // re-publish re-declares the position it will actually occupy, so a stale one
    // cannot anchor the node wrong). Returns its msgid, 0 when it did not pack.
    // One GridSync attempt's payload (S7: the delivery service frames, places
    // through place_grid_sync_, matches the ack and runs the ladder).
    bool LORAListener::encode_grid_sync_(bool retry, const DeliveryRequest::FrameFn &frame)
    {
      hubframes::Down op_message = {};

      // Chosen BEFORE the frame is built, because the frame declares it.
      const int64_t planned_t0 = this->grid_sync_planned_t0_();
      this->gridsync_planned_t0_ = planned_t0;   // place_grid_sync_ aims at it
      this->gridsync_republishes_ = retry ? (uint8_t) (this->gridsync_republishes_ + 1) : 0;

      GridSync gs = GRID_SYNC__INIT;
      this->fill_grid_sync_(&gs, planned_t0);

      op_message.cmd_case = hubframes::DL_GRIDSYNC;
      op_message.gridsync = &gs;

      if (!frame(&op_message))
      {
        ESP_LOGE(TAG, "[%s] failed to pack GridSync", this->get_name().c_str());
        return false;
      }
      return true;
    }

    void LORAListener::place_grid_sync_(const MacFrame &f)
    {
      const int64_t planned_t0 = this->gridsync_planned_t0_;
      // PLACED, so the declaration above is true. Not through the bare send():
      // the node solves its anchor from where this frame ARRIVES, so an
      // unplaced GridSync displaces every mark the node will ever arm — by an
      // arbitrary amount, since "whenever the queue drains" is unbounded, and
      // past the guard band that is a missed window every round rather than a
      // late one.
      //
      // The copy count is asked for EXPLICITLY. send_aligned_ reads
      // copies == 0 as "no shape requested" and may then reduce the frame to a
      // single copy under §4.6 — which would be exactly wrong here: a node
      // being told about the grid is by definition not yet on it, so it is
      // sweeping a free-running window and would miss a single copy most of
      // the time. Copy 0 lands on the mark and the rest follow at the burst
      // stride; the node backs out its own copy's offset with burstIndex.
      TxPolicy p;
      p.copies = this->parent_->defaultBurstCopies();
      // The mark the declaration above was computed from, so the frame goes
      // out where it says it does.
      p.earliest_us = (planned_t0 != 0)
                          ? loratiming::fireInstantUs(planned_t0, 0) : 0;
      this->send_aligned_(f.bytes.data(), f.bytes.size(), p, f.seal);
      {
        const int64_t published_us = esp_timer_get_time();
        this->last_gridsync_publish_us_ = (published_us > 0) ? published_us : 1;
      }
      ESP_LOGI(TAG, "[%s] GridSync PUBLISHED (slot %u)", this->get_name().c_str(),
               (unsigned) this->grid_slot_);
    }

    // The node confirmed the grid (any of the published msgids): it is on the
    // grid, stop re-publishing.
    bool LORAListener::on_grid_sync_acked_(uint32_t msgid)
    {
      ESP_LOGI(TAG, "[%s] GridSync acknowledged (msgid=%u, %u re-publishes)",
               this->get_name().c_str(), (unsigned) msgid,
               (unsigned) this->gridsync_republishes_);
      this->last_gridsync_ack_us_ = esp_timer_get_time();
      return true;
    }

    // The ladder is spent. The session is fine; the node is simply still in Mode A
    // and a late ack is no longer recognised (the ids are dropped).
    void LORAListener::on_grid_sync_failed_()
    {
      ESP_LOGE(TAG, "[%s] GridSync not acknowledged after %u re-publishes — "
                    "the node stays in Mode A until its next login",
               this->get_name().c_str(), (unsigned) kGridSyncMaxRepublishes);
      this->mac().acks.clear();
      if (modebsup::reloginAfterSpentLadder(this->s_since_(this->last_regrid_us_),
                                            this->last_gridsync_ack_us_ != 0,
                                            this->s_since_(this->last_forced_relogin_us_)))
      {
        ESP_LOGW(TAG, "[%s] Not one ack since the reboot — the node is deaf to our downlinks; forcing a LOGIN",
                 this->get_name().c_str());
        this->last_forced_relogin_us_ = std::max<int64_t>(esp_timer_get_time(), 1);
        this->set_timeout("gridsync_wedge_relogin", 0, [this]() { this->do_login_and_arm_retry_(); });
      }
    }

    // Withdrawing the grid is NOT a delivery: a plain, unplaced burst, never
    // awaited or re-published.
    void LORAListener::withdraw_grid_sync_()
    {
      // A withdrawal takes the bound with it: no published interval means no
      // promotion, which is where a node that has just lost its grid belongs.
      this->published_resync_max_s_ = 0;
      hubframes::Down op_message = {};
      GridSync gs = GRID_SYNC__INIT;
      gs.enable = false;
      op_message.cmd_case = hubframes::DL_GRIDSYNC;
      op_message.gridsync = &gs;

      MacFrame f;
      if (!this->mac_frame_(&op_message, &f))
      {
        ESP_LOGE(TAG, "[%s] failed to pack GridSync", this->get_name().c_str());
        return;
      }
      // A withdrawal ends any wait for a confirmation of the grid.
      this->dlv_cancel_(CH_MAC);
      // The withdrawal stays a plain burst. It is broadcast on the demote
      // path, and it is addressed to nodes whose anchor may already be wrong
      // — placing it on a mark they may not agree about is the one thing it
      // must not depend on.
      this->mac0_send_(f.bytes.data(), f.bytes.size(), TxPolicy{}, f.seal);
      ESP_LOGI(TAG, "[%s] GridSync WITHDRAWN (slot %u)", this->get_name().c_str(),
               (unsigned) this->grid_slot_);
    }

    // B3's entry point. See the banner on the declaration for why this is an
    // explicit opt-in rather than automatic.
    void LORAListener::enable_timed_mode(bool on)
    {
      if (this->parent_ == nullptr)
        return;
      if (on == this->timed_mode_enabled_)
      {
        ESP_LOGI(TAG, "[%s] timed mode %s (unchanged)", this->get_name().c_str(), on ? "ON" : "OFF");
        return;
      }

      if (on && !this->parent_->setupDone())
      {
        this->restore_timed_mode_at_boot_();
        return;
      }

      this->timed_mode_enabled_ = on;
      this->mbs_restart_();
      this->start_mode_b_supervisor_();

      if (on)
      {
        // Order matters. The grid must be RUNNING before a GridSync is built,
        // because the frame declares its own position on it (`txRound`/`txSlot`)
        // and a node anchors on exactly that — publishing a position the frame
        // does not occupy would anchor the node wrong, permanently, and it
        // would look like a clock fault.
        this->parent_->startGrid();
        this->set_grid_aligned(true);
        this->send_grid_sync(true);
        // Confidence starts at zero every time the grid is (re)published: the
        // node has to earn single-shot again from observed uplinks.
        this->belief_               = timedmode::HubBelief{};
        this->last_in_slot_us_      = 0;
        this->have_in_slot_confirm_ = false;
        ESP_LOGW(TAG, "[%s] timed mode ON — grid published, slot %u, alignment on",
                 this->get_name().c_str(), (unsigned) this->grid_slot_);
      }
      else
      {
        // Withdraw BEFORE dropping alignment, so the withdrawal itself still
        // goes out on the grid the node is listening on. Dropping alignment
        // first would send it into a window the node has stopped opening — the
        // one frame that must not be missed, missed.
        this->send_grid_sync(false);
        this->set_grid_aligned(false);
        ESP_LOGW(TAG, "[%s] timed mode OFF — grid withdrawn, back to Mode A",
                 this->get_name().c_str());
      }
    }

    void LORAListener::broadcast_grid_demote()
    {
      // Broadcast, because after a restart the hub may not yet know which nodes
      // exist — and every one of them is holding an anchor that is now wrong.
      //
      // GridDemote, not GridSync{enable=false}, and that was a real defect
      // rather than a tidy-up. This frame is sent on a FRESH BOOT, when no
      // session exists, so v1_frame_ framed it in the clear —
      // and the node's plaintext gate refuses every non-LOGIN, non-beacon frame
      // while it holds a session resumed from NVS. The one frame that must not
      // be missed was dropped by exactly the nodes it was aimed at.
      //
      // Security review finding 6: the missed-mark counter this comment used
      // to cite as the fallback that "actually works" was retired
      // 2026-09-20 — no such fallback exists on the node any more. The
      // node's own handleGridDemote() rate-limits honoring a demote to once
      // per hour (kGridDemoteRateLimitMs), so if THIS broadcast is also
      // missed (plaintext-gate drop above, or simple air loss) and the hub
      // reboots again inside that hour (the known double-boot pattern), the
      // node sits on a dead anchor until SyncStale eventually notices or the
      // next NTP-gated startup LOGIN fires — not broken, but not prompt
      // either, and not something to assume is covered elsewhere.
      //
      // GridDemote has no fields and is exempt from that gate. What makes the
      // exemption safe is that its only effect is to throw an anchor away:
      // Mode A is three windows per round against Mode B's one, so an
      // unauthenticated sender can only make a node listen MORE.
      // Unplaced, and as a burst: it is broadcast, and it is addressed to nodes
      // whose anchor is by definition already wrong -- so there is no mark to
      // place it on that any of them would agree with. FCNT 0: a broadcast
      // belongs to no per-node sequence (spec 2.4).
      MacFrame f;
      if (this->v1_demote_frame_(&f))
        this->mac0_send_(f.bytes.data(), f.bytes.size(), TxPolicy{}, mac2::FrameSeal{});
      ESP_LOGW(TAG, "[%s] startup: broadcast grid demote sent (%u B)",
               this->get_name().c_str(), (unsigned) f.bytes.size());
    }

    bool LORAListener::send_aligned_(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal)
    {
      return this->send_aligned_(buf, len, TxPolicy{}, seal);
    }

    // The mark a placed frame for this node would go out on.
    //
    // Split out of send_aligned_ so a producer that must DECLARE its position
    // can ask the same question the placement will answer. Two computations of
    // "the next clear mark" would be two answers the moment either moved.
    // Clear BY INTERVAL (the five-argument nextClearT0ForSlotUs), not after the
    // end of the latest reservation: a single-copy mark before a queued beacon
    // is free air. Every placed downlink comes through here, and a skipped
    // mark costs a whole round of latency.
    int64_t LORAListener::nextPlacementT0_(int64_t now_us, int copies, size_t len,
                                           bool expects_reply) const
    {
      int64_t t0 = this->parent_->nextClearT0ForSlotUs(this->grid_slot_, now_us,
                                                       copies, len, expects_reply);
      if (t0 <= this->last_placed_t0_us_)
        t0 = this->parent_->nextClearT0ForSlotUs(this->grid_slot_,
                                                 this->last_placed_t0_us_ + 1,
                                                 copies, len, expects_reply);
      return t0;
    }

    bool LORAListener::send_aligned_(const uint8_t *buf, size_t len,
                                     const TxPolicy &in_policy, const mac2::FrameSeal &seal)
    {
      if (buf == nullptr || len == 0)
        return false;

      // §4.6, rules 1-4. ONE copy instead of seventeen, but only while the hub
      // has recent, authenticated evidence that this node's window will be open
      // when the copy arrives: a phase report from its last decrypted beacon,
      // inside the guard on both error and spread, over enough samples to mean
      // something, on the crystal, with no reboot or session change since and
      // no single shot currently unacked. Any of those missing and this is the
      // burst it always was — the policy is written as a list of reasons to
      // fall BACK, so a state nobody thought about costs airtime, not a command.
      TxPolicy policy = in_policy;
      const timedmode::HubBelief belief = this->hubBeliefNow_();
      const bool single_shot =
          (policy.copies == 0) &&    // the caller has not asked for a shape
          (this->txPolicyNow() == timedmode::TxPolicy::SingleShot);
      if (single_shot)
      {
        policy.copies = 1;
        ESP_LOGI(TAG, "[%s] single shot: phase %+d +/-%d us over %u samples, "
                      "reported %us ago",
                 this->get_name().c_str(), (int) belief.phase_err_us,
                 (int) belief.phase_spread_us, (unsigned) belief.phase_samples,
                 (unsigned) belief.confirmation_age_s);
      }
      // The SHAPE of the frame just placed, which is not the same thing as the
      // shape of the tracked command.
      //
      // This used to assign sent_single_shot directly, and Rule 4 reads
      // that to decide whether an unacked command costs this node its
      // single-shot confidence. Every other producer through send_aligned_ then
      // clobbered it: a grid publication asks for the burst explicitly, so
      // publishing a grid CLEARED the flag and Rule 4 could not fire for a
      // single shot that really had gone unacked. Routing TimeSync and
      // ScheduleConfig through here would have made that routine rather than
      // rare. The tracked-op senders copy it into sent_single_shot
      // themselves, so the flag belongs to the command it describes.
      this->last_placed_single_shot_ = single_shot;
      // Was the optimistic switch what made this a single shot? Judged by asking
      // the SAME ladder with the switch withheld: if its only remaining refusal
      // is ConfirmationStale, the bypass was used. This decides whether a miss
      // gets the short first ack wait (SingleShotAckWait.h); a single shot that
      // the hub was entitled to on fresh evidence keeps the normal wait.
      {
        timedmode::HubBelief strict = belief;
        strict.optimistic_single_shot = false;
        this->last_placed_optimistic_ =
            single_shot &&
            timedmode::txRefusalFor(strict, timedgrid::kGuardUs,
                                    this->published_resync_max_s_) ==
                timedmode::TxRefusal::ConfirmationStale;
      }

      if (!this->grid_aligned_ || !this->parent_->gridStarted())
      {
        return this->mac0_send_(buf, len, policy, seal);
      }

      // The next mark that is CLEAR of the hub's own burst, not simply the next
      // mark. A 17-copy burst denies 31 of the 32 slots in its round (measured;
      // section 4.5), so aiming at the next T0 would place this frame inside a
      // burst roughly whenever one is running — the node would hear the burst
      // it is not addressed by and nothing it is.
      //
      // Handed to the TRANSMIT QUEUE as earliest_us rather than held here in an
      // ESPHome timeout. Two reasons, and the second is the one that mattered:
      //
      //   * The queue owns the radio, so it can FIRE at the mark (B5's
      //     prepare/fire split) instead of merely handing the frame over near
      //     it. A set_timeout could only ever deliver it to the back of the
      //     queue at roughly the right time.
      //   * The named timeout meant a second command for the same node before
      //     the first had fired REPLACED it — a dropped command, silently,
      //     whenever two arrived inside one round. Placing the second at the
      //     FOLLOWING mark keeps both: one frame per mark is the invariant that
      //     was wanted, and dropping was never the way to get it.
      const int64_t now = esp_timer_get_time();
      // A caller that has already chosen the mark keeps it. send_grid_sync does
      // that because it has to DECLARE the round the frame goes out in, and a
      // declaration computed from one mark and transmitted on another is how
      // the two ends came to disagree about which round is a beacon round.
      // The real shape of THIS frame. policy.copies may still be 0 here, meaning
      // "the normal burst" — burstEndUs_ resolves that to txSlotsPerRound, so
      // passing it through is both correct and the conservative reading.
      const int64_t t0 = (policy.earliest_us != 0)
                             ? loratiming::t0FromFireInstantUs(policy.earliest_us, 0)
                             : this->nextPlacementT0_(now, policy.copies, len,
                                                      policy.expects_reply);
      // earliest_us is a FIRE instant; t0 is where the node expects the frame's
      // reference to land. They differ by kDownlinkPreambleToT0Us, and the node's
      // window is built around the T0, so the conversion belongs here. See
      // TxPolicy::earliest_us; d_tx_ramp is 0 for the reason given in
      // send_into_class_a_window_.
      policy.earliest_us = loratiming::fireInstantUs(t0, 0);
      policy.on_mark     = true;   // t0 IS this node's mark: a phase measurement
      ESP_LOGD(TAG, "[%s] placing %u B at slot %u T0, %lld ms out",
               this->get_name().c_str(), (unsigned) len,
               (unsigned) this->grid_slot_, (long long) ((t0 - now) / 1000));

      // The mark is spent only if the frame actually entered the queue. send()
      // drops silently when the buffer pool is exhausted or the handoff queue
      // is full; recording the mark anyway would push the NEXT command for this
      // node a further round out to make room for a frame that does not exist.
      if (this->mac0_send_(buf, len, policy, seal))
      {
        this->last_placed_t0_us_ = t0;
        return true;
      }

      ESP_LOGW(TAG, "[%s] placed frame dropped by the transmit queue — mark "
                    "%lld not consumed",
               this->get_name().c_str(), (long long) t0);
      return false;
    }

    // §4.6, the hub half: an uplink OBSERVED IN ITS SLOT, not a claim.
    //
    // The node cannot answer this about itself. It knows when it transmitted
    // and it knows the grid it was told about, so its answer is a restatement
    // of its own belief — "a beacon saying I am ready says nothing about where
    // its window actually landed" (TimedModePolicy.h). The hub measures the
    // arrival against ITS OWN grid, which is the only independent evidence
    // that the two clocks agree well enough to stop sending seventeen copies.
    //
    // The comparison is against the node's uplink INSTANT — its mark plus the
    // uplink offset the hub itself published in GridSync — and the tolerance is
    // the same guard the node's phase tracking uses, so "in slot" means the
    // same thing at both ends.
    void LORAListener::noteUplinkPlacement_(int64_t t0_uplink_us)
    {
      if (this->parent_ == nullptr || t0_uplink_us <= 0)
        return;
      // No grid, nothing to be in slot of. Deliberately does NOT reset the
      // count: a node that has not been given a grid is not failing to hit it.
      if (!this->grid_aligned_ || !this->parent_->gridStarted())
        return;

      // Where the node's mark for this uplink was. Subtract the offset first,
      // then find the nearest mark: an uplink a hair early belongs to the mark
      // ahead of it, and taking the NEXT mark would charge it a whole round.
      const int64_t mark_x = t0_uplink_us - (int64_t) LORAListener::kUplinkOffsetUs;
      const int64_t next   = this->parent_->nextT0ForSlotUs(this->grid_slot_, mark_x);
      const int64_t prev   = next - (int64_t) timedgrid::kRoundUs;
      const int64_t mark   = ((next - mark_x) <= (mark_x - prev)) ? next : prev;
      const int64_t err    = mark_x - mark;

      const bool in_slot = (err <= (int64_t) timedgrid::kGuardUs) &&
                           (err >= -(int64_t) timedgrid::kGuardUs);

      if (!in_slot)
      {
        if (this->belief_.in_slot_acks != 0)
          ESP_LOGW(TAG, "[%s] uplink %lld us out of slot %u — confidence reset",
                   this->get_name().c_str(), (long long) err,
                   (unsigned) this->grid_slot_);
        this->belief_.in_slot_acks = 0;
        return;
      }

      if (this->belief_.in_slot_acks < 0xFFFFFFFFu)
        this->belief_.in_slot_acks++;
      this->last_in_slot_us_      = esp_timer_get_time();
      this->have_in_slot_confirm_ = true;
      // A confirmation is exactly what these three were waiting for.
      this->belief_.rebooted_since_confirm = false;
      this->belief_.session_changed        = false;
      this->belief_.single_shot_unacked    = false;
      ESP_LOGD(TAG, "[%s] uplink in slot %u (err %lld us), %u consecutive",
               this->get_name().c_str(), (unsigned) this->grid_slot_,
               (long long) err, (unsigned) this->belief_.in_slot_acks);
    }

    // §4.6's promotion evidence, from the node's own phase statistics.
    //
    // The node measured where the HUB's frames landed relative to the marks it
    // armed for. That is an observation of this link by the end that has to
    // hear it — not a claim of readiness — and it answers the question
    // single-shot actually turns on: will the window be open when the one copy
    // arrives? In-slot uplink placement was a proxy for that and a poor one,
    // because it is produced by the node's TRANSMIT path (CAD, a burst-end
    // deferral, a random pre-transmit backoff), none of which has anything to
    // do with when the node ARMS. See TimedModePolicy.h.
    void LORAListener::notePhaseReportForTest(uint32_t rtc_slow_src, int32_t err_us,
                                              int32_t spread_us, uint32_t samples,
                                              uint32_t outside_guard, bool node_timed_rx)
    {
      ::PhaseReport pr = PHASE_REPORT__INIT;
      pr.rtcslowsrc    = rtc_slow_src;
      pr.errus         = err_us;
      pr.spreadus      = spread_us;
      pr.samples       = samples;
      pr.outsideguard  = outside_guard;
      pr.timedrxactive = node_timed_rx;
      this->notePhaseReport_(&pr);
    }

    void LORAListener::notePhaseReport_(const ::PhaseReport *pr)
    {
      // ABSENT IS NOT EMPTY, the same rule the pending mask is built on. A node
      // whose firmware predates the field sends no PhaseReport at all, and
      // overwriting a good report with zeros on its account would be a silent
      // demotion; leaving the belief untouched lets confirmation_age_s expire
      // it on its own, which is the honest outcome.
      if (pr == nullptr)
        return;

      this->belief_.phase_reported      = (pr->samples > 0);
      this->belief_.phase_err_us        = pr->errus;
      this->belief_.phase_spread_us     = pr->spreadus;
      this->belief_.phase_samples       = pr->samples;
      this->belief_.phase_outside_guard = pr->outsideguard;
      // The node's own decision, which single-shot follows (TimedModePolicy.h).
      this->belief_.node_timed_rx       = pr->timedrxactive;
      this->belief_.node_demotion       = pr->demotionreason;
      this->belief_.rtc_src =
          (pr->rtcslowsrc == 2u) ? timedmode::RtcSlowSrc::Crystal
        : (pr->rtcslowsrc == 1u) ? timedmode::RtcSlowSrc::InternalRc
                                 : timedmode::RtcSlowSrc::Unknown;
      this->last_phase_report_us_ = esp_timer_get_time();
      this->have_phase_report_    = true;

      // U-2. Logged only when it GROWS: the node throttles its own warning and
      // this report arrives on every ack, so an unconditional line here would
      // restate a static number several times a minute. A rising count is the
      // event — it means marks are passing unarmed, and every window-mark rate
      // taken over that stretch has an empty denominator rather than a clean
      // one.
      if (pr->rxbusyskips > this->node_rx_busy_skips_)
        ESP_LOGW(TAG, "node skipped %u RX window(s) on a busy radio (%u since its boot) "
                      "— WMR over this period is measuring fewer marks than it appears to",
                 (unsigned) (pr->rxbusyskips - this->node_rx_busy_skips_),
                 (unsigned) pr->rxbusyskips);
      this->node_rx_busy_skips_ = pr->rxbusyskips;
      this->have_rx_busy_skips_ = true;

      // This report IS the fresh confirmation the three guards were waiting
      // for. They used to be cleared by an observed in-slot uplink, and that
      // has to move with the evidence: leaving them keyed on placement would
      // hold every node at Burst forever now that placement no longer gates
      // promotion. txPolicyFor still judges the report on its merits, so a
      // beacon carrying a bad phase clears these and is refused anyway.
      this->belief_.rebooted_since_confirm = false;
      this->belief_.session_changed        = false;
      this->belief_.single_shot_unacked    = false;
    }

    timedmode::HubBelief LORAListener::hubBeliefNow_() const
    {
      timedmode::HubBelief b = this->belief_;
      b.grid_enabled   = this->timed_mode_enabled_ && this->grid_aligned_;
      // U-5: computed here rather than stored, for the same reason
      // confirmation_age_s is — it is a fact about the passage of time, and a
      // stored copy would be right only at the instant it was written. It was
      // a txPolicyFor guard that nothing ever set, so single-shot was decided
      // without the one question that asks whether the node is still there.
      b.beacon_missed  = this->beacon_overdue_();
      // Known only once the node has told us, in a beacon we decrypted.
      b.firmware_known = (this->node_fw_version_ != 0);

      // The confirmation is the node's PHASE REPORT, not an observed uplink
      // placement — see TimedModePolicy.h's kPromotionPhaseSamples for why the
      // evidence changed. So freshness is the age of the beacon that carried
      // it. have_phase_report_ is an explicit flag rather than a zero check on
      // the timestamp: the harness clock starts at zero, and "never" and "at
      // boot" are different answers.
      b.confirmation_age_s =
          !this->have_phase_report_
              ? 0xFFFFFFFFu
              : (uint32_t) ((esp_timer_get_time() - this->last_phase_report_us_) / 1000000);

      // The operator's opt-in, stamped here for the same reason the four fields
      // above are: it is a fact about the hub's configuration NOW, not an event
      // this node reported. Keeping it out of belief_ leaves that struct purely
      // evidential — a setter on it would be the "claim" §4.6 exists to reject.
      //
      // What it permits is narrow: ConfirmationStale, and nothing else. See
      // HubBelief::optimistic_single_shot for the arithmetic and for why
      // NoPhaseReport is deliberately NOT bypassable.
      b.optimistic_single_shot = this->optimistic_single_shot_;
      return b;
    }

    // C2: place a reply inside the node's RX1 window.
    //
    // The node's Class A windows hang off T0_uplink, which it measures from its
    // own TxDone. The hub's estimate of that same instant is Bx's receive
    // stamp — last_rx_t0_us() — so a reply aimed at T0_uplink + kRx1DelayUs
    // lands where the node is listening WITHOUT either end knowing the other's
    // clock. That is the whole appeal of Class A, and it is why this works
    // before the hub's stamp is anywhere near the +/-1 ms it eventually wants.
    //
    // What it replaces: set_timeout(..., 750, ...) into the ordinary burst
    // path, so copies landed at 750 + 88*i ms. Worked through against this
    // plan's own arm points, BOTH windows are empty — RX1 is [982.8, 1012.2]
    // and the nearest copies are at 926 and 1014 — and the node then sleeps.
    // Even a fortunate alignment would only be a 33.5 % hit. Class A needs a
    // single copy at a precisely known offset; a burst is the opposite
    // construction.
    //
    // Returns false when the hub has no usable stamp for this node's uplink, in
    // which case the caller must fall back to today's behaviour rather than
    // guess an origin.
    LORAListener::ClassAPlacement
    LORAListener::send_into_class_a_window_(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal)
    {
      if (this->parent_ == nullptr || buf == nullptr || len == 0)
        return ClassAPlacement::NotSendable;

      // Does this node actually OPEN a Class A window? Aiming one copy at a
      // window that is not there is strictly worse than the burst it replaces:
      // an interactive node sweeps a 29.44 ms window through a 500 ms period,
      // so a single copy at a fixed offset hits it about 6 % of the time,
      // while the 17-copy burst spans 1408 ms and hits it every time.
      //
      // This mirrors the node's own predicate exactly (CmdDispatcher's
      // noteUplinkSent: auto_mode && !grid_.active). Both halves must agree or
      // the frame is aimed into silence, so this asks the two questions the
      // node asks, from the hub's own knowledge:
      //
      //   - the node last told us it was in AUTO, in a beacon we decrypted;
      //     node_mode_ defaults to MODE_INTERACTIVE, so an unheard node
      //     declines rather than guesses.
      //   - the node is not on a grid. A gridded node arms off the hub's
      //     marks, not off its own uplink, so Class A does not apply to it.
      //
      // Declining is not a failure: the caller falls back to the burst, which
      // is what this path did before C2 existed.
      const bool node_is_class_a =
          (this->node_mode_ == (uint32_t) NODE_MODE__MODE_AUTO) &&
          !(this->timed_mode_enabled_ && this->grid_aligned_);
      if (!node_is_class_a)
      {
        ESP_LOGD(TAG, "[%s] not a Class A node (mode %u, grid %d) — burst",
                 this->get_name().c_str(), (unsigned) this->node_mode_,
                 (int) (this->timed_mode_enabled_ && this->grid_aligned_));
        return ClassAPlacement::NotClassA;
      }

      // This node's own last uplink, committed in commit_rx_msgid_ — never the
      // tracker's live stamp, which by now belongs to whichever node
      // transmitted most recently.
      const int64_t t0_uplink = this->last_uplink_t0_us_;
      if (t0_uplink <= 0)
        return ClassAPlacement::NoUplinkStamp;

      const int64_t now = esp_timer_get_time();

      // RX1, then RX2 — the node opens BOTH, and until now the hub only ever
      // aimed at the first.
      //
      // The node's own sequence (classa::nextAction) opens RX2 whenever RX1
      // passed WITHOUT DATA, which is exactly the case the hub lands in when
      // its reply is not ready in time: nothing was sent into RX1, so the node
      // is listening at +2 s. The old code logged "RX1 already past" and fell
      // back to a 17-copy burst — 1408 ms of air aimed at a node whose window
      // is 29.44 ms wide and whose position the hub knows exactly. RX2 is the
      // second chance the protocol already specifies, and the node was already
      // arming it.
      //
      // The T0 we want the node to see is one window delay after its own
      // uplink's T0. This is a RECEIVER-side instant, and the two ends have no
      // shared anchor here to absorb a constant offset the way the grid does —
      // both windows hang off an absolute delay from the node's own transmit —
      // so the conversion below is not cosmetic.
      //
      // T0 is kDownlinkPreambleToT0Us (4160 us) AFTER the first chirp leaves
      // the antenna, and earliest_us is the instant lora_tx() is called.
      // Handing the wanted T0 over as-is puts the frame 4160 us late at the
      // node -- 30 % of the 14080 us guard, spent before the link has done
      // anything.
      //
      // d_tx_ramp is passed as 0 on purpose. Its only provenance is a
      // commented-out delayMicroseconds(220) (section 12.1 lists it as
      // unmeasured), and fireInstantUs takes it as a parameter precisely so a
      // placeholder cannot be mistaken for a measurement. Correcting the known
      // 4160 and leaving the unknown ~220 as residual is strictly better than
      // correcting neither.
      //
      // A window is only reachable if the queue can still fire ON it: popDue
      // releases a placed frame kPrepareLeadUs early, so a target closer than
      // that cannot be hit and asking for it would put the copy late by
      // whatever the shortfall is. Tightening that test costs nothing now that
      // declining RX1 means trying RX2 rather than giving up on placement.
      const int64_t rx1_target =
          loratiming::fireInstantUs(t0_uplink + (int64_t) classa::kRx1DelayUs, 0);
      const int64_t rx2_target =
          loratiming::fireInstantUs(t0_uplink + (int64_t) classa::kRx2DelayUs, 0);
      const int64_t earliest_usable = now + txqueue::kPrepareLeadUs;

      int64_t     target    = 0;
      int64_t     wanted_t0 = 0;
      const char *window    = nullptr;
      if (rx1_target >= earliest_usable)
      {
        target    = rx1_target;
        wanted_t0 = t0_uplink + (int64_t) classa::kRx1DelayUs;
        window    = "RX1";
      }
      else if (rx2_target >= earliest_usable)
      {
        target    = rx2_target;
        wanted_t0 = t0_uplink + (int64_t) classa::kRx2DelayUs;
        window    = "RX2";
        ESP_LOGD(TAG, "[%s] RX1 unreachable by %lld us — aiming at RX2",
                 this->get_name().c_str(),
                 (long long) (earliest_usable - rx1_target));
      }
      else
      {
        // Both gone. The node is asleep or about to be, and a burst now would
        // be 1408 ms of air with nothing listening for it. This is reported as
        // its OWN reason, not as a generic decline: the caller must be able to
        // tell it apart from "I cannot place this node", which wants the
        // opposite answer.
        ESP_LOGW(TAG, "[%s] both Class A windows past (RX2 by %lld us) — "
                      "the node is asleep",
                 this->get_name().c_str(),
                 (long long) (earliest_usable - rx2_target));
        return ClassAPlacement::WindowsPast;
      }

      TxPolicy p;
      p.copies      = 1;              // a burst is the opposite construction
      p.stride_ms   = 0;
      p.earliest_us = target;
      p.priority    = 0;              // Immediate: a missed window is a lost
                                      // reply, and the next one is a wake away
      if (!this->mac0_send_(buf, len, p, seal))
      {
        // The pool refused it: nothing was aimed at the window, so nothing is
        // counted as offered (the hit rate divides by this), and the caller
        // must not report the frame as sent (review HB13).
        ESP_LOGW(TAG, "[%s] reply for %s dropped by the transmit queue", this->get_name().c_str(),
                 window);
        return ClassAPlacement::Dropped;
      }

      ESP_LOGI(TAG, "[%s] reply aimed at %s: T0_uplink %lld, wanted T0 %lld, "
                    "fire %lld (hub stamp +/-%u us)",
               this->get_name().c_str(), window, (long long) t0_uplink,
               (long long) wanted_t0, (long long) target,
               (unsigned) this->last_uplink_unc_us_);

      // Mode C's ledger, stage 0: the hub AIMED a frame at a window it believed
      // open. Counted here rather than in send_timesync so every caller of this
      // path is counted, present and future — the question is "was a frame
      // placed", not "which feature placed it".
      this->class_a_stats_.offered++;

      return ClassAPlacement::Placed;
    }

    // Everything the hub believed about the node's grid describes a grid it no
    // longer holds. The promoted belief and the GridSync acknowledgement are the
    // two the supervisor reads (mbs_inputs_).
    void LORAListener::forget_node_grid_()
    {
      this->last_gridsync_ack_us_          = 0;
      this->belief_.rebooted_since_confirm = true;
      this->belief_.in_slot_acks           = 0;
      this->have_in_slot_confirm_          = false;
      this->belief_.phase_reported         = false;
      this->belief_.phase_samples          = 0;
      this->belief_.node_timed_rx          = false;
      this->have_phase_report_             = false;
    }

    bool LORAListener::regrid_due_() const
    {
      const uint32_t since =
          (this->last_gridsync_publish_us_ == 0)
              ? 0xFFFFFFFFu
              : (uint32_t) ((esp_timer_get_time() - this->last_gridsync_publish_us_) / 1000000);
      return modebsup::regridDue(this->regrid_pending_, this->gridSyncAwaitingAck(), since);
    }

    // Publish the grid again, through the same delivery glue as every other publish
    // (Mac channel, ladder, encrypted under the live session). Only for a node the
    // supervisor would manage: Timed Mode ON, not auto-mode. Idempotent through
    // regrid_due_(). False when nothing was scheduled.
    bool LORAListener::regrid_node_(const char *why)
    {
      if (!this->mbs_supervised_() || !this->regrid_due_())
        return false;
      ESP_LOGW(TAG, "[%s] %s — forgetting its grid acknowledgement, GridSync in %u ms",
               this->get_name().c_str(), why, (unsigned) modebsup::kRegridDelayMs);
      this->forget_node_grid_();
      this->last_regrid_us_ = std::max<int64_t>(esp_timer_get_time(), 1);
      this->regrid_pending_ = true;
      this->set_timeout("gridsync_regrid", modebsup::kRegridDelayMs, [this]() { this->regrid_fire_(); });
      return true;
    }

    void LORAListener::regrid_fire_()
    {
      this->regrid_pending_ = false;
      if (this->timed_mode_enabled_)
        this->send_grid_sync(true);
    }

    void LORAListener::regrid_after_reboot_(const ::NodeWakeBeacon *b, bool reset_changed)
    {
      const bool timed_rx = (b->phase != nullptr) && b->phase->timedrxactive;
      if (!modebsup::beaconMeansGridLost((uint32_t) b->reason, b->resetreason, reset_changed, timed_rx))
        return;
      if (this->regrid_node_("Node rebooted without a login (grid is RAM)"))
        this->mbs_restart_();
    }

  }  // namespace lora_tracker
}  // namespace esphome
