// lora_client_bench.cpp -- the listener's bench streams: MAC ping / echo,
// the drift test, ModeTest (its marks, report handling) and the FCNT blocks
// they spend from the esp_timer task (stage 5 split of lora_client.cpp, moved
// verbatim). Threading: docs/threading-model.md -- the timer callbacks here
// run on the esp_timer task and touch only their own stream state and the
// reservations (under session_tx_mutex_).
#include "lora_client_internal.h"

namespace esphome
{
  namespace lora_tracker
  {

    // ACK / POSITION / BEACON, for both the encrypted and the plaintext path.
    //
    // These were two separate ladders. They were compared line by line before
    // merging — identical. (The `awaiting_ack` guard that let a position report
    // count as delivery confirmation was removed on both paths at once, review
    // F3, 2026-10-16: see dispatch_payload_.) If the plaintext path
    // should ever accept LESS than the encrypted one (a defensible security
    // position), split them again deliberately rather than by drift.
    // -----------------------------------------------------------------------
    // MAC-0 ping / echo — the hub half (mac-layer.md sections 5 and 6).
    //
    // Deliberately shaped like the drift test, because that shape was arrived
    // at by paying for the alternatives: the frame goes through the NORMAL
    // transmit queue rather than sendPacketOnce() from the timer callback (that
    // bypasses TxDone handling and the return-to-RX in the tracker main loop,
    // and left the SX1278 stuck in TX with the hub silent until restarted), and
    // the next frame is built immediately after a send rather than inside the
    // interval being measured.
    //
    // What is different: one copy per mark, not seventeen. Seventeen copies of
    // one mark is not one mark, and until the per-frame TxPolicy existed there
    // was no way to say so.
    // -----------------------------------------------------------------------
    void LORAListener::build_mac_ping_frame_()
    {
      hubframes::Down op_message = {};
      uint32_t msgid = 0;

      // MAC-2 OFF (the default): a DEDICATED counter, deliberately not
      // incrTxMessageId().
      //
      // Two reasons, both load-bearing. (1) That call advances the session TX
      // id the node's replay window tracks. The node hears only a fraction of
      // the pings in windowed RX — and with MAC-1 off for control frames it
      // does not advance its rx_id_ at all — so hub and node diverge by one per
      // ping. At 250 ms spacing a 300 s run emits 1200 pings and the next real
      // command lands outside the node's 1024-wide accept window: every command
      // rejected as a "huge jump" until the next LOGIN. (2) incrTxMessageId()
      // persists to NVS, and this runs in an esp_timer task while the ESPHome
      // main loop owns that backend — hundreds of writes per run, off-loop.
      //
      // MAC-2 ON: the FCNT is the CTR counter-block input, so it MUST come from
      // the block start_mac_ping reserved for this session (mac_ping_fcnts_);
      // the dedicated ping counter could collide with an ordinary command's
      // FCNT, which is keystream reuse. A block that ran out, or was dropped by
      // a re-LOGIN / key change, ends the encrypted pings: the rest of the run
      // is plaintext (the node refuses those in session), never sealed.
      uint32_t reserved = 0;
      if (this->mac_ping_crypto_ && this->session_confirmed_)
        reserved = this->take_reserved_fcnt_(&this->mac_ping_fcnts_);
      const bool encrypt_this_one = reserved != 0;
      msgid = encrypt_this_one ? reserved : ++this->mac_ping_msgid_;
      if (this->mac_ping_crypto_ && !encrypt_this_one)
      {
        this->mac_ping_crypto_ = false;   // say it once
        ESP_LOGW(TAG, "[%s] MAC ping: no reserved FCNT left for this session — the "
                      "rest of the run is plaintext", this->get_name().c_str());
      }

      MacControl mc = MAC_CONTROL__INIT;
      mc.kind     = MAC_CONTROL__KIND__MAC_PING;
      mc.seq      = ++this->mac_ping_seq_;
      mc.wantecho = this->mac_ping_want_echo_;

      // Padding sweeps time on air across the frame table. The FIFO read is per
      // byte on both ends, so a turnaround measured at one length says nothing
      // about another.
      uint8_t pad[128];
      uint32_t pad_len = this->mac_ping_pad_bytes_;
      if (pad_len > sizeof(pad))
        pad_len = sizeof(pad);
      if (pad_len)
      {
        memset(pad, 0xAB, pad_len);
        mc.pad.len  = pad_len;
        mc.pad.data = pad;
      }

      op_message.cmd_case   = hubframes::DL_MACCONTROL;
      op_message.maccontrol = &mc;

      MacFrame f;
      if (!this->mac_frame_(&op_message, msgid, encrypt_this_one, &f))
      {
        ESP_LOGE(TAG, "[%s] MAC ping: failed to pack", this->get_name().c_str());
        this->mac_ping_frame_len_ = 0;
        return;
      }
      if (encrypt_this_one && !this->sealed_in_epoch_(f, this->mac_ping_fcnts_))
      {
        ESP_LOGW(TAG, "[%s] MAC ping: keys changed while sealing — frame dropped",
                 this->get_name().c_str());
        this->mac_ping_frame_len_ = 0;
        return;
      }
      if (f.bytes.size() > sizeof(this->mac_ping_frame_))
      {
        ESP_LOGE(TAG, "[%s] MAC ping frame too large (%u B) — not sent",
                 this->get_name().c_str(), (unsigned) f.bytes.size());
        this->mac_ping_frame_len_ = 0;
      }
      else
      {
        memcpy(this->mac_ping_frame_, f.bytes.data(), f.bytes.size());
        this->mac_ping_frame_len_ = f.bytes.size();
        this->mac_ping_seal_      = f.seal;
      }
    }

    void LORAListener::mac_ping_timer_cb_(void *arg)
    {
      auto *self = static_cast<LORAListener *>(arg);
      const CallbackBusy busy(self->mac_ping_cb_busy_);
      if (!self->mac_ping_active_ || self->mac_ping_frame_len_ == 0)
        return;

      // ONE copy, PLACED on this node's mark. This is the whole reason B-1 had
      // to come first.
      //
      // It went through parent_->send() BARE until 2026-09-20: no earliest_us,
      // so the frame left whenever the queue drained. Against a Mode B node —
      // one 29.44 ms window per 1500 ms round — a single unplaced copy lands
      // about 2 % of the time, so a run reported near-total loss and was
      // measuring the SEND path rather than reception.
      //
      // Why every other bare sender here is unaffected: a 17-copy burst spans
      // 1408 ms of a 1500 ms round and hits the window by brute force. A frame
      // reaching a Mode B node must be PLACED or BURSTED; one copy unplaced is
      // neither aimed nor wide, and that combination is unique to this path and
      // the drift test.
      //
      // send_aligned_ also stamps on_mark, which is the node's licence to read
      // the arrival as a phase sample (CmdDispatcher, "ONLY A FRAME THE HUB
      // PLACED ON THIS NODE'S MARK"). So a placed ping both measures reception
      // and feeds promotion.
      //
      // copies is set EXPLICITLY: §4.6's single-shot decision applies only when
      // the caller asks for no shape (policy.copies == 0), and a ping must be
      // exactly one copy whatever the hub currently believes about the node.
      TxPolicy p;
      p.copies        = 1;
      p.stride_ms     = 0;
      p.expects_reply = self->mac_ping_want_echo_;
      // Placed by the timer's own placement (send_placed_from_timer_), never
      // send_aligned_: that writes last_placed_* and reads the belief, which the
      // loop owns and place_ctl_ reads right after its own placement (HB2).
      if (self->send_placed_from_timer_(self->mac_ping_frame_, self->mac_ping_frame_len_, p,
                                        self->mac_ping_seal_, &self->mac_ping_last_mark_t0_us_))
      {
        self->mac_stats_.pings_offered++;
        self->mac_stats_.last_seq_sent = self->mac_ping_seq_;
      }
      else
      {
        // Counted only once it has actually entered the queue. pings_offered
        // documents itself as "marks handed to the transmit queue", and a yield
        // computed against frames that were never queued is not a measurement —
        // the same reason serviceBeacon logs a drop rather than recording the
        // round as queued.
        ESP_LOGW(TAG, "[%s] MAC ping seq %u was not queued — pool exhausted?",
                 self->get_name().c_str(), (unsigned) self->mac_ping_seq_);
      }

      self->build_mac_ping_frame_();
    }

    void LORAListener::handle_mac_echo_(const ::MacControl *echo)
    {
      if (echo == nullptr || echo->kind != MAC_CONTROL__KIND__MAC_ECHO)
        return;

      this->mac_stats_.echoes_rx++;

      // Count marks that were offered and never came back. Keyed on seq, which
      // is exact and gap-tolerant: a frame lost to the air leaves a hole rather
      // than shifting every later sample.
      if (this->mac_stats_.have_echo && echo->seq > this->mac_stats_.last_seq_echoed + 1)
        this->mac_stats_.echo_seq_gaps +=
            echo->seq - this->mac_stats_.last_seq_echoed - 1;

      this->mac_stats_.last_seq_echoed = echo->seq;
      this->mac_stats_.have_echo = true;

      ESP_LOGD(TAG, "[%s] MAC echo seq=%u (offered %u, echoed %u, gaps %u)",
               this->get_name().c_str(), (unsigned) echo->seq,
               (unsigned) this->mac_stats_.pings_offered,
               (unsigned) this->mac_stats_.echoes_rx,
               (unsigned) this->mac_stats_.echo_seq_gaps);
    }

    void LORAListener::start_mac_ping(uint32_t duration_s, uint32_t grid_ms,
                                      bool want_echo, uint32_t pad_bytes,
                                      bool crypto)
    {
      if (this->mac_ping_active_)
        return;

      this->mac_ping_active_    = true;
      this->mac_ping_want_echo_ = want_echo;
      this->mac_ping_pad_bytes_ = pad_bytes;
      this->reset_mac_stats();

      // MAC-2: reserve the whole run's msgids from the SESSION's own tx-id
      // space, here, in this (main-loop, button-press) call, with exactly one
      // NVS save — never per-frame from the esp_timer callback. Without a
      // session there is no base nonce to seal anything under, so crypto
      // silently falls back to plaintext, matching every other downlink here.
      this->mac_ping_crypto_ = crypto && this->session_confirmed_;
      if (crypto && !this->session_confirmed_)
        ESP_LOGW(TAG, "[%s] MAC ping: no confirmed session — crypto requested but "
                      "pings will be plaintext", this->get_name().c_str());
      // A run that does not want crypto must not inherit a PREVIOUS run's
      // unused reserved range: build_mac_ping_frame_'s encrypt_this_one already
      // gates on mac_ping_crypto_ first, but clearing the range here removes
      // the stale values outright rather than leaving them for that gate alone
      // to keep meaning nothing.
      if (!this->mac_ping_crypto_)
      {
        std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
        this->mac_ping_fcnts_.invalidate();
      }
      else
      {
        const uint32_t ms = (grid_ms > 0) ? grid_ms : 1100;
        // +2: the START-to-first-tick delay and the ceiling division both round
        // down against the real cadence; the range is a floor, not a promise.
        const uint32_t frames = (duration_s * 1000UL) / ms + 2;
        this->reserve_fcnt_block_(frames, &this->mac_ping_fcnts_);
      }

      if (this->mac_ping_timer_ == nullptr)
      {
        const esp_timer_create_args_t args = {
            .callback              = &LORAListener::mac_ping_timer_cb_,
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "mac_ping",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &this->mac_ping_timer_) != ESP_OK)
        {
          ESP_LOGE(TAG, "[%s] MAC ping: timer create failed", this->get_name().c_str());
          this->mac_ping_active_ = false;
          return;
        }
      }

      this->build_mac_ping_frame_();
      esp_timer_start_periodic(this->mac_ping_timer_, (uint64_t) grid_ms * 1000ULL);
      this->set_timeout("mac_ping_end", duration_s * 1000,
                        [this]() { this->stop_mac_ping(); });

      // PRECONDITION, said out loud rather than discovered in the numbers.
      //
      // send_aligned_ places only when this node is grid-aligned and the hub's
      // grid is running; otherwise it falls through to an unplaced send. That
      // is the right fallback for a Mode A node (which sweeps a free-running
      // window) but it silently turns a Mode B run into the ~2 % lottery this
      // path was just fixed to escape. A whole run's worth of numbers is an
      // expensive way to learn the grid was off.
      if (!this->grid_aligned_ || this->parent_ == nullptr ||
          !this->parent_->gridStarted())
      {
        ESP_LOGW(TAG, "[%s] MAC ping: grid not aligned/started — pings will be "
                      "UNPLACED. Fine for Mode A; against a Mode B node this "
                      "measures the send path, not reception.",
                 this->get_name().c_str());
      }

      // The ping msgids are their own sequence and will not satisfy the node's
      // replay window, so a run needs MAC-1 off on the node (M3). Said out loud
      // because the failure — every ping silently rejected — looks exactly like
      // a reception problem.
      ESP_LOGI(TAG, "[%s] MAC ping START: %u s, grid %u ms, single copy, "
                    "echo %s, pad %u B (requires MAC-1 disabled on the node)",
               this->get_name().c_str(), (unsigned) duration_s, (unsigned) grid_ms,
               want_echo ? "on" : "off", (unsigned) pad_bytes);
    }

    void LORAListener::stop_mac_ping()
    {
      if (!this->mac_ping_active_)
        return;
      this->mac_ping_active_ = false;
      if (this->mac_ping_timer_ != nullptr)
        esp_timer_stop(this->mac_ping_timer_);
      wait_callback_idle_(this->mac_ping_cb_busy_);
      this->cancel_timeout("mac_ping_end");
      this->mac_ping_frame_len_ = 0;

      // The two sides of the funnel, reported together. A yield computed from
      // one end alone is not a measurement.
      ESP_LOGI(TAG, "[%s] MAC ping STOP: offered %u, echoed %u, gaps %u",
               this->get_name().c_str(), (unsigned) this->mac_stats_.pings_offered,
               (unsigned) this->mac_stats_.echoes_rx,
               (unsigned) this->mac_stats_.echo_seq_gaps);
    }

    // The report is RECOMPUTED here, not on the node.
    //
    // Independence claim I1 (test-plan.md section 10.4): the node is the one
    // thing that cannot be trusted to judge its own reception, so it ships raw
    // counters and every rate in mac-layer.md section 6 is derived off-node.
    // MacFunnel.h owns that arithmetic and is compiled into both ends, so the
    // rates here are the same function the node would have used — just not the
    // node's opinion of them.
    void LORAListener::handle_mode_test_report_(const ::ModeTestReport *rep)
    {
      if (rep == nullptr)
        return;

      macfunnel::Counters c{};
      c.detected         = rep->detected;
      c.crc_valid        = rep->crcvalid;
      c.addressed        = rep->addressed;
      c.counter_accepted = rep->counteraccepted;
      c.mic_valid        = rep->micvalid;
      c.duplicates       = rep->duplicates;
      c.mic_failures     = rep->micfailures;
      c.windows_armed    = rep->windowsarmed;
      c.windows_hit      = rep->windowshit;

      // seqGaps is the honest denominator here. The node cannot know how many
      // marks were OFFERED (that is the hub's number) or how many reached the
      // air (that is a witness receiver's, which is gap I2), but first..last
      // plus the gaps in between is a lower bound on what was sent, and it does
      // not depend on MAC-1 being enabled.
      const uint32_t expected = (rep->seqlast >= rep->seqfirst)
                              ? (rep->seqlast - rep->seqfirst + 1) : 0;

      // `expected` is the denominator, not this->mt_seq_. The hub's own count
      // includes marks it queued before the node started listening and after it
      // stopped; dividing by that would report the node's late arrival as
      // packet loss. Over the window the node actually observed, first..last is
      // the honest span. What neither number can supply is how many frames
      // reached the AIR — that needs a witness receiver, and its absence is the
      // acknowledged gap I2, not something to paper over with a hub-side count.

      // 1024, not 512: measured 2026-09-14 the line ran out at "residual 3" — the
      // one number Mode B's pass line is judged on — and it is not published
      // anywhere else, so the run's residual could not be read at all.
      char buf[1024];
      std::snprintf(buf, sizeof(buf),
          "mode=%u prod=%d counter=%d crypto=%d | seq %u..%u exp %u gaps %u | "
          "detected %u crcValid %u addressed %u counterAcc %u micValid %u | "
          "windows %u/%u | "
          "FER_link %u ppm WMR %u ppm DUP %u ppm MIC_FAIL %u ppm | "
          "phaseErr p50 %d p99 %d max %d n %u | turnaround p50 %d p99 %d n %u | "
          "armResidual p99 %d | oneShot p99 %d n %u | rtcSlowSrc %u | "
          "tick %u Hz cpu %u MHz "
          "elapsed %u s refusal %u | ppm %d n %u period %d us | residual %d ppm n %u",
          (unsigned) rep->mode, (int) rep->powerprofileproduction,
          (int) rep->counteron, (int) rep->cryptoon,
          (unsigned) rep->seqfirst, (unsigned) rep->seqlast,
          (unsigned) expected, (unsigned) rep->seqgaps,
          (unsigned) rep->detected, (unsigned) rep->crcvalid,
          (unsigned) rep->addressed, (unsigned) rep->counteraccepted,
          (unsigned) rep->micvalid,
          // Printed raw, because WMR cannot speak for them: lossPpm returns 0
          // for a zero denominator, so a run that armed NO windows reports a
          // window miss rate of 0 ppm — the same number a flawless run gives.
          // With mode now derived from the node's applied state, `mode=2
          // windows 0/0` is the signature of timed RX enabled but never active
          // (phase never became trustworthy), which is a real Mode B outcome
          // and has to be legible as one rather than as a perfect score.
          (unsigned) rep->windowsarmed, (unsigned) rep->windowshit,
          (unsigned) macfunnel::ferLinkPpm(c, expected),
          (unsigned) macfunnel::wmrPpm(c),
          (unsigned) macfunnel::dupPpm(c), (unsigned) macfunnel::micFailPpm(c),
          rep->phaseerrus ? rep->phaseerrus->p50 : 0,
          rep->phaseerrus ? rep->phaseerrus->p99 : 0,
          rep->phaseerrus ? rep->phaseerrus->max : 0,
          rep->phaseerrus ? (unsigned) rep->phaseerrus->n : 0u,
          rep->turnaroundus ? rep->turnaroundus->p50 : 0,
          rep->turnaroundus ? rep->turnaroundus->p99 : 0,
          rep->turnaroundus ? (unsigned) rep->turnaroundus->n : 0u,
          rep->armresidualus ? rep->armresidualus->p99 : 0,
          rep->oneshoterrorus ? rep->oneshoterrorus->p99 : 0,
          // HW-8's miss count is windowsArmed - n, and there is no dedicated
          // miss field: n is the number of one-shots that actually FIRED, so
          // without it a bounded p99 says nothing about how many windows the
          // timer silently never opened. A single miss fails that gate.
          rep->oneshoterrorus ? (unsigned) rep->oneshoterrorus->n : 0u,
          // Gates Mode B entirely (test-plan.md 10.7). It was consumed into
          // belief_ and never printed or published, so bench-runbook.md 0's
          // "confirm rtcSlowSrc from the beacon, not the schematic" had no
          // observable to read. 2 = external crystal; anything else means this
          // run's Mode B numbers describe a node that could not hold phase.
          (unsigned) rep->rtcslowsrc,
          (unsigned) rep->tickratehz, (unsigned) rep->cpufreqmhz,
          (unsigned) rep->elapseds, (unsigned) rep->armrefusal,
          // Mode B's goal as a number: the node's clock rate against this hub,
          // under the run's power profile. Printed because it was carried on
          // the wire and nowhere else — and the node did not fill it at all
          // until the run-scoped fit existed.
          (int) rep->ppmestimate, (unsigned) rep->ppmsamples,
          (int) rep->measuredperiodus,
          // Mode B pass line (|residual| < 20): the drift left after the node
          // applies its learned rate. ppm above stays the RAW rate.
          (int) rep->residualppm, (unsigned) rep->residualsamples);

      // The same numbers, kept as numbers. The log line above is for a human
      // reading a console; these are for Home Assistant, where B3's gate lives:
      // "reception >= Mode A over a week" is a week of history, and a 512-byte
      // string truncated to HA's 255-character limit is not a measurement.
      ModeTestSummary sum;
      sum.valid                    = true;
      sum.mode                     = rep->mode;
      sum.arm_refusal              = rep->armrefusal;
      sum.elapsed_s                = rep->elapseds;
      sum.windows_armed            = rep->windowsarmed;
      sum.windows_hit              = rep->windowshit;
      sum.fer_link_ppm             = macfunnel::ferLinkPpm(c, expected);
      sum.wmr_ppm                  = macfunnel::wmrPpm(c);
      sum.dup_ppm                  = macfunnel::dupPpm(c);
      sum.mic_fail_ppm             = macfunnel::micFailPpm(c);
      sum.phase_p50_us             = rep->phaseerrus ? rep->phaseerrus->p50 : 0;
      sum.phase_p99_us             = rep->phaseerrus ? rep->phaseerrus->p99 : 0;
      sum.turnaround_p99_us        = rep->turnaroundus ? rep->turnaroundus->p99 : 0;
      sum.power_profile_production = rep->powerprofileproduction;
      sum.ppm_estimate             = rep->ppmestimate;
      sum.ppm_samples              = rep->ppmsamples;
      sum.measured_period_us       = rep->measuredperiodus;
      sum.residual_ppm             = rep->residualppm;
      sum.residual_samples         = rep->residualsamples;
      this->mode_test_summary_     = sum;

      this->last_mode_test_report_ = buf;
      ESP_LOGW(TAG, "[%s] ModeTest REPORT: %s", this->get_name().c_str(), buf);
    }

    bool LORAListener::reserve_fcnt_block_(uint32_t n, mac1::FcntReservation *r)
    {
      const mac::Addr peer = static_cast<mac::Addr>(this->short_address_);
      const uint32_t first = (n > 0) ? this->incrTxMessageId() : 0;   // one persist
      if (first == 0 || first >= framecrypto::kMsgidCtrLimit)
      {
        std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
        r->invalidate();
        return false;
      }
      uint32_t last = first + (n - 1);
      if (last < first || last >= framecrypto::kMsgidCtrLimit)
        last = framecrypto::kMsgidCtrLimit - 1;
      const uint32_t epoch = s_keys().keyEpoch(peer);   // keys_mutex_ never nests inside ours
      {
        std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
        this->set_counters_no_write_(last, this->seq_.rxId());   // skip the block
        r->grant(first, last, epoch);
      }
      this->save_state_(true);   // ...and persist its end
      return true;
    }

    uint32_t LORAListener::take_reserved_fcnt_(mac1::FcntReservation *r)
    {
      const uint32_t epoch = s_keys().keyEpoch(static_cast<mac::Addr>(this->short_address_));
      std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
      return r->take(epoch);
    }

    // A frame sealed from a reserved FCNT must carry the reservation's key
    // epoch: otherwise the keys moved between take() and the seal and the
    // ciphertext must never reach the air.
    bool LORAListener::sealed_in_epoch_(const MacFrame &f, const mac1::FcntReservation &r)
    {
      std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
      return f.seal.sealed && f.seal.key_epoch == r.keyEpoch();
    }

    void LORAListener::invalidate_fcnt_reservations_()
    {
      std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
      this->mac_ping_fcnts_.invalidate();
      this->mt_fcnts_.invalidate();
      this->drift_fcnts_.invalidate();
    }

    // A test stream's frame: a fresh FCNT on the loop (block == nullptr: the
    // START / STOP frames), or the next one of the run's reserved block (the
    // marks and ticks, built on the esp_timer task -- no Sequencer, no persist).
    // False when the block is spent, dropped, or the keys moved under the seal.
    bool LORAListener::pack_test_frame_(hubframes::Down *op, mac1::FcntReservation *block,
                                        MacFrame *f)
    {
      if (block == nullptr)
        return this->mac_frame_(op, f);
      const uint32_t fcnt = this->take_reserved_fcnt_(block);
      if (fcnt == 0)
        return false;
      if (!this->mac_frame_(op, fcnt, this->session_confirmed_, f))
        return false;
      if (f->seal.sealed && !this->sealed_in_epoch_(*f, *block))
      {
        f->bytes.clear();
        return false;
      }
      return true;
    }

    bool LORAListener::send_placed_from_timer_(const uint8_t *buf, size_t len, TxPolicy policy,
                                               const mac2::FrameSeal &seal,
                                               int64_t *last_mark_t0_us)
    {
      if (!this->grid_aligned_ || !this->parent_->gridStarted())
        return this->mac0_send_(buf, len, policy, seal);
      const int64_t now = esp_timer_get_time();
      int64_t t0 = this->parent_->nextClearT0ForSlotUs(this->grid_slot_, now, policy.copies, len,
                                                       policy.expects_reply);
      if (t0 <= *last_mark_t0_us)
        t0 = this->parent_->nextClearT0ForSlotUs(this->grid_slot_, *last_mark_t0_us + 1,
                                                 policy.copies, len, policy.expects_reply);
      policy.earliest_us = loratiming::fireInstantUs(t0, 0);
      policy.on_mark     = true;   // the node's own mark: a phase sample
      if (!this->mac0_send_(buf, len, policy, seal))
        return false;
      *last_mark_t0_us = t0;
      return true;
    }

    void LORAListener::wait_callback_idle_(const std::atomic<bool> &busy)
    {
      // The esp_timer task outranks the loop, so a callback that is running
      // finishes while we yield; this returns at once when none is.
      while (busy.load())
        std::this_thread::yield();
    }

    // -----------------------------------------------------------------------
    // Drift test (bench only)
    //
    // The hub half of the test exists because a drift fit needs a RULER: a
    // burst emitted on a known, regular grid. In normal interactive operation
    // the hub only transmits when it has something to say, so the node would
    // get a fit at unpredictable intervals or not at all.
    //
    // Cost note: each grid frame is a full 17-copy burst (~330 ms of airtime).
    // At the default 15 s grid that is ~2 % occupancy, which is why the grid is
    // seconds rather than milliseconds.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Drift test — a hardware-timed ruler
    //
    // WHY THIS IS NOT A BURST ANY MORE
    //
    // The first version reused the ordinary 17-copy burst as the ruler. That
    // burst is paced by vTaskDelayUntil, and this hub runs
    // CONFIG_FREERTOS_HZ=1000 — so every copy is released on a 1 ms tick
    // boundary. Over the 1.4 s span of a burst, +-1 ms of placement error is
    // ~700 ppm of slope error, and the node duly reported +-1500 ppm of
    // scatter for a crystal that cannot exceed +-20 ppm. The RULER was
    // quantised; no amount of receiver precision could fix it.
    //
    // So: one frame per tick, paced by esp_timer (microsecond resolution, APB
    // derived) instead of the FreeRTOS tick, over a long baseline. The frame
    // for the NEXT tick is built immediately after each send, so the timer
    // callback does nothing but transmit — packing work cannot leak into the
    // interval being measured.
    //
    // Frames are PLAINTEXT: encryption time varies with payload and would sit
    // between the timer firing and the radio starting. Same payload every
    // time, so whatever latency remains is constant and affects the offset,
    // never the slope.
    // -----------------------------------------------------------------------
    void LORAListener::drift_timer_cb_(void *arg)
    {
      auto *self = static_cast<LORAListener *>(arg);
      const CallbackBusy busy(self->drift_cb_busy_);
      if (!self->drift_test_active_ || self->drift_frame_len_ == 0)
        return;

      // Hand the PRE-BUILT frame to the normal transmit queue.
      //
      // NOT sendPacketOnce() straight from here. That bypasses the TxDone
      // handling and the return-to-RX that live in the tracker main loop, and
      // it left the SX1278 stuck in TX: every later beginPacket failed and the
      // hub went silent until it was restarted. Observed 2026-08-31 — the node
      // received nothing at all from the moment the test was triggered.
      //
      // A PER-FRAME copies=1 makes this one frame instead of seventeen, so the
      // proven path is reused rather than duplicated — and, unlike the old
      // global setBurstCopies(1), it cannot leak onto an ordinary command that
      // a user happens to send during the test.
      self->mac0_send_(self->drift_frame_, self->drift_frame_len_,
                       TxPolicy{/*copies=*/1, /*stride_ms=*/0}, self->drift_seal_);

      // Build the next one now, in the 99% of the period that is idle.
      self->build_drift_frame_(true, &self->drift_fcnts_);
    }

    // ---- ModeTest ----------------------------------------------------
    //
    // Structurally identical to the drift test above, and that is the point:
    // the two behaviours encoded in drift_timer_cb_ were paid for on hardware
    // and are not worth rediscovering.
    //
    // Where it differs: every frame carries the FULL request, not just a tick.
    // The node arms on the first ModeTest it hears, and it is in windowed RX
    // when the test starts — a ~29 ms window every 500 ms — so it will miss
    // most of them. A frame that only said "tick" would leave a node that
    // missed the first one waiting for a repeat that never comes.
    void LORAListener::build_mode_test_frame_(bool enable, mac1::FcntReservation *block)
    {
      hubframes::Down op_message {};

      ModeTest mt = MODE_TEST__INIT;
      mt.enable           = enable;
      mt.durations        = this->mt_duration_s_;
      mt.mode             = (ModeTest__Mode) this->mt_mode_;
      mt.gridperiodms     = this->mt_grid_ms_;
      mt.copies           = this->mt_copies_;
      // The hub's own knob keeps the human polarity — "keep the profile" is
      // what the button and the YAML argument mean — and the wire carries the
      // inverse, because proto3's zero has to be the safe answer. See the
      // reserved field 7 in blinds.proto.
      mt.droppowerprofile = !this->mt_keep_power_profile_;
      mt.enablecounter    = this->mt_enable_counter_;
      mt.enablecrypto     = this->mt_enable_crypto_;
      mt.macecho          = this->mt_mac_echo_;
      mt.armoffsetus      = this->mt_arm_offset_us_;
      // The ruler mark, separate from msgid. msgid is also the CTR counter-block input
      // and the replay-filter key, so the test must be able to retransmit
      // without touching either; seq is free to be a plain monotonic index, and
      // a frame lost to the air leaves a GAP in it rather than shifting every
      // later sample.
      mt.seq              = this->mt_seq_++;

      op_message.cmd_case = hubframes::DL_MODETEST;
      op_message.modetest = &mt;

      // Encrypted, like every other command, and for the same two reasons the
      // drift frame is: the node rejects unencrypted commands once a session
      // exists, and exempting this one would let an unauthenticated frame pin a
      // node in a test mode. The node refuses to ARM without a session anyway
      // (ModeTestPolicy::armRefusal), so this is the second of two locks.
      MacFrame f;
      if (this->pack_test_frame_(&op_message, block, &f))
      {
        if (f.bytes.size() <= sizeof(this->mt_frame_))
        {
          memcpy(this->mt_frame_, f.bytes.data(), f.bytes.size());
          this->mt_frame_len_ = f.bytes.size();
          this->mt_seal_      = f.seal;
        }
        else
        {
          ESP_LOGE(TAG, "[%s] ModeTest frame too large (%u B)",
                   this->get_name().c_str(), (unsigned) f.bytes.size());
          this->mt_frame_len_ = 0;
        }
      }
      else
      {
        this->mt_frame_len_ = 0;
      }
    }

    void LORAListener::mode_test_timer_cb_(void *arg)
    {
      auto *self = static_cast<LORAListener *>(arg);
      const CallbackBusy busy(self->mt_cb_busy_);
      if (!self->mode_test_active_ || self->mt_frame_len_ == 0)
        return;

      // The normal transmit queue, with a per-frame copy count. Never
      // sendPacketOnce() from here — see the banner on build_mode_test_frame_.
      TxPolicy p{/*copies=*/(int) self->mt_copies_, /*stride_ms=*/0};
      // Nobody answers a mark: no response window held or reserved after it.
      p.expects_reply = false;

      // D4: IN MODE B THE MARK GOES ON THE NODE'S GRID MARK.
      //
      // MEASURED 2026-09-13. Marks used to leave whenever this timer fired, so
      // in Mode B they sat at an arbitrary, fixed phase against the grid —
      // every one of 14 phase samples at -651 ms, ~46 guard bands out.
      // phaseTrustworthy() could never pass, timed RX never armed (`windows
      // 0/0`), and HW-8's one-shot histogram stayed empty: the test's own marks
      // were the frames that made Mode B unreachable.
      //
      // Aimed the way send_aligned_ aims — the next mark CLEAR of the hub's own
      // bursts — but tracked here in mt_last_mark_t0_us_ rather than in
      // last_placed_t0_us_: this callback runs in the esp_timer task, and that
      // member belongs to the ESPHome loop. The timer period IS the round, so
      // consecutive ticks land on consecutive marks; the guard keeps a jittered
      // tick from aiming two marks at one.
      //
      // Mode A stays unplaced on purpose: its swept period (1093 ms) is what
      // lets a free-running window catch it at all.
      //
      // MODE 4 (SWEEP) BELONGS HERE TOO — added 2026-09-19. It is a timed-window
      // run, distinguished only by a deliberate arm offset, so its marks must
      // land on the node's mark like Mode B's. Gated on mode 2 alone, every
      // sweep mark left unplaced and carried neither onMark nor fireStamped —
      // and BOTH the mark-hit path and the phase-sample commit
      // (CmdDispatcher.cpp:4858) require one of those. So a sweep resolved no
      // marks and committed no samples: windows 14/1 on a 120 s point, then
      // windows 0/0 -> Demotion::NoPhase -> no REPORT at all. Exactly the
      // failure the note above records for Mode B on 2026-09-13, inherited by
      // the mode that was never added to the condition.
      //
      // With placement on, the 1093 ms timer tick is deferred to the next clear
      // slot T0, so ticks collapse onto ~one mark per round — which is what a
      // sweep needs. The period itself can stay incommensurate; it is the
      // PLACEMENT, not the period, that was missing.
      if ((self->mt_mode_ == 2 || self->mt_mode_ == 4) && self->parent_->gridStarted())
      {
        const int64_t now = esp_timer_get_time();
        // Kept for the log line below: mt_last_mark_t0_us_ is overwritten before
        // it runs, so the spacing between consecutive marks — the one number
        // that tells a placed run (~1500 ms) from an unplaced one (~1093 ms) —
        // would otherwise be gone by the time it is printed.
        const int64_t prev_mark = self->mt_last_mark_t0_us_;
        // By interval: a mark before a queued beacon is clear (2026-09-15).
        // HOLD a tick whose mark is already queued a whole round beyond the
        // grid's next mark. MEASURED 2026-10-18 (S0-S7 checkpoint): another
        // node's frame pushed one mark two rounds out, and the "mark after the
        // last" guard below then kept EVERY later mark that far ahead, because
        // the timer period is the round (mode 4's 1093 ms timer is worse: it
        // runs ahead by a round every ~3.7 ticks). Each round of lead pins one
        // tracker pool buffer until it fires; at 4 of 5 held, beacons and marks
        // were dropped and each dropped mark was a seq gap at the node. The
        // built frame is kept, so the next tick sends it: no seq gap.
        const int64_t round_us = (int64_t) timedgrid::kRoundUs;
        const int64_t grid_t0  = self->parent_->nextT0ForSlotUs(self->grid_slot_, now);
        if (self->mt_last_mark_t0_us_ >= grid_t0 + round_us)
        {
          ESP_LOGI(TAG, "[%s] mark: mode %u held (last mark t0=%lld is a round ahead)",
                   self->get_name().c_str(), (unsigned) self->mt_mode_,
                   (long long) self->mt_last_mark_t0_us_);
          return;
        }
        const int copies = (int) self->mt_copies_;
        const size_t len = self->mt_frame_len_;
        int64_t t0 = self->parent_->nextClearT0ForSlotUs(self->grid_slot_, now, copies,
                                                         len, false);
        if (t0 <= self->mt_last_mark_t0_us_)
          t0 = self->parent_->nextClearT0ForSlotUs(self->grid_slot_,
                                                   self->mt_last_mark_t0_us_ + 1,
                                                   copies, len, false);
        if (t0 > 0)
        {
          p.earliest_us = loratiming::fireInstantUs(t0, 0);
          p.on_mark     = true;   // the node's own mark: its phase sample
          self->mt_last_mark_t0_us_ = t0;
        }
        // Placement is invisible from the hub otherwise: this path logs nothing,
        // and a sweep that reports a 1093 ms mark period cannot be told from a
        // placed one without it. Three separate theories were argued from
        // inference on 2026-09-19 before this line existed.
        ESP_LOGI(TAG, "[%s] mark: mode %u placed=%d t0=%lld dt=%lld ms",
                 self->get_name().c_str(), (unsigned) self->mt_mode_,
                 (int) (t0 > 0), (long long) t0,
                 (long long) ((t0 - prev_mark) / 1000));
      }
      else
      {
        ESP_LOGI(TAG, "[%s] mark: mode %u UNPLACED (gridStarted=%d)",
                 self->get_name().c_str(), (unsigned) self->mt_mode_,
                 (int) self->parent_->gridStarted());
      }
      self->mac0_send_(self->mt_frame_, self->mt_frame_len_, p, self->mt_seal_);

      // Build the next one now, in the idle part of the period.
      self->build_mode_test_frame_(true, &self->mt_fcnts_);
    }

    void LORAListener::start_mode_test(uint32_t duration_s, uint32_t grid_ms,
                                       uint32_t mode, uint32_t copies,
                                       bool keep_power_profile,
                                       bool enable_counter, bool enable_crypto,
                                       bool mac_echo, int32_t arm_offset_us)
    {
      if (this->parent_ == nullptr)
        return;

      if (grid_ms < 200)
        grid_ms = 200;
      if (copies < 1)  copies = 1;
      if (copies > 17) copies = 17;

      this->mode_test_active_      = true;
      this->mbs_owns_test_         = this->mbs_starting_;
      this->mt_duration_s_         = duration_s;
      this->mt_grid_ms_            = grid_ms;
      this->mt_mode_               = mode;
      this->mt_copies_             = copies;
      this->mt_keep_power_profile_ = keep_power_profile;
      this->mt_enable_counter_     = enable_counter;
      this->mt_enable_crypto_      = enable_crypto;
      this->mt_mac_echo_           = mac_echo;
      this->mt_arm_offset_us_      = arm_offset_us;
      this->mt_seq_                = 0;
      this->mt_last_mark_t0_us_    = 0;
      this->last_mode_test_report_.clear();

      ESP_LOGI(TAG, "[%s] ModeTest START: %u s, mode %u, grid %u ms, %u copies, "
                    "counter %d crypto %d echo %d production-profile %d",
               this->get_name().c_str(), (unsigned) duration_s, (unsigned) mode,
               (unsigned) grid_ms, (unsigned) copies, (int) enable_counter,
               (int) enable_crypto, (int) mac_echo, (int) keep_power_profile);

      // The START goes out as a NORMAL full burst, whatever `copies` says for
      // the grid frames. The node is still in windowed RX — a ~29 ms window
      // every 500 ms, a 5.9 % duty cycle — so a single-copy START has roughly a
      // 6 % chance of being heard. The drift test learned this the hard way:
      // the node received nothing at all for eight minutes.
      this->build_mode_test_frame_(true);
      if (this->mt_frame_len_ > 0)
        this->mac0_send_(this->mt_frame_, this->mt_frame_len_, TxPolicy{}, this->mt_seal_);
      // Every mark's FCNT, reserved here on the loop (HB1): the marks are built
      // on the esp_timer task, which must not touch the Sequencer. The hub stops
      // the run 5 s after duration_s; +4 covers the first mark and rounding.
      this->reserve_fcnt_block_(((duration_s + 5) * 1000) / grid_ms + 4, &this->mt_fcnts_);

      if (this->mode_test_timer_ == nullptr)
      {
        const esp_timer_create_args_t args = {
            .callback = &LORAListener::mode_test_timer_cb_,
            .arg      = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name     = "modetestgrid",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &this->mode_test_timer_) != ESP_OK)
        {
          ESP_LOGE(TAG, "[%s] ModeTest: timer create failed — no marks", this->get_name().c_str());
          this->mode_test_timer_ = nullptr;
          this->mode_test_active_ = false;
          return;
        }
      }
      esp_timer_stop(this->mode_test_timer_);

      // Let the bursted START land before the grid begins. One burst round is
      // 1.5 s; 3 s is comfortable margin.
      //
      // THE NODE RELIES ON THIS NUMBER: a node that missed the START joins on an
      // early mark and computes how much of the run is left from it
      // (modetest::kHubFirstMarkDelayMs in ModeTestPolicy.h). Change both or
      // neither.
      this->set_timeout("modetest_grid_arm", 3000, [this, grid_ms]() {
        if (!this->mode_test_active_)
          return;
        this->build_mode_test_frame_(true, &this->mt_fcnts_);
        esp_timer_start_periodic(this->mode_test_timer_,
                                 (uint64_t) grid_ms * 1000);
      });

      // The hub stops itself too. The node owns its own deadline and will end
      // the test regardless, but a hub that kept transmitting into a node that
      // had already stopped would look like a link failure in the next test.
      this->set_timeout("modetest_stop", (duration_s + 5) * 1000,
                        [this]() { this->stop_mode_test(); });
    }

    void LORAListener::stop_mode_test()
    {
      this->stop_mode_test_(true);
    }

    // send_stop = false is the supervisor's quiet abort (see mbs_abort_warmup_).
    void LORAListener::stop_mode_test_(bool send_stop)
    {
      if (!this->mode_test_active_)
        return;
      this->mode_test_active_ = false;

      this->cancel_timeout("modetest_grid_arm");
      this->cancel_timeout("modetest_stop");
      if (this->mode_test_timer_ != nullptr)
        esp_timer_stop(this->mode_test_timer_);
      // esp_timer_stop does not wait for a callback already running; one
      // mid-build would race the STOP frame for mt_frame_ / mt_seq_ (HB2).
      wait_callback_idle_(this->mt_cb_busy_);

      // One last frame with enable=false, so a node that is still running ends
      // now rather than at its own deadline.
      if (send_stop)
      {
        this->build_mode_test_frame_(false);
        if (this->mt_frame_len_ > 0)
          this->mac0_send_(this->mt_frame_, this->mt_frame_len_, TxPolicy{}, this->mt_seal_);
      }

      ESP_LOGI(TAG, "[%s] ModeTest STOP (%u marks sent)",
               this->get_name().c_str(), (unsigned) this->mt_seq_);
    }

    void LORAListener::build_drift_frame_(bool enable, mac1::FcntReservation *block)
    {
      hubframes::Down op_message {};
      // msgid is the ruler mark (mac_frame_ allocates it). The node indexes
      // samples by it, so a frame lost to the air leaves a gap instead of
      // shifting every later sample.

      DriftTest dt = DRIFT_TEST__INIT;
      dt.enable       = enable;
      dt.durations    = this->drift_test_duration_s_;
      dt.gridperiodms = this->drift_test_grid_ms_;

      op_message.cmd_case  = hubframes::DL_DRIFTTEST;
      op_message.drifttest = &dt;

      MacFrame f;
      // ENCRYPTED, like every other command.
      //
      // Plaintext was tried first, to keep variable-time crypto out of the
      // timing path. It does not belong there anyway: this frame is built
      // during the ~999 ms of idle BETWEEN ticks, so packing and encryption
      // are already outside the interval being measured. On the node, decrypt
      // happens after the ISR has captured the timestamp, so it cannot bias
      // the measurement either.
      //
      // And plaintext simply does not work: the node rejects unencrypted
      // commands once a session exists ("session requires encryption"), so
      // every drift frame was silently dropped and the test never started.
      // Exempting DriftTest would let an unauthenticated frame pin a node in
      // ~11 mA continuous RX — a battery-drain vector on a 1.2 mA device.
      if (this->pack_test_frame_(&op_message, block, &f) &&
          f.bytes.size() <= sizeof(this->drift_frame_))
      {
        memcpy(this->drift_frame_, f.bytes.data(), f.bytes.size());
        this->drift_frame_len_ = f.bytes.size();
        this->drift_seal_      = f.seal;
      }
      else
      {
        this->drift_frame_len_ = 0;   // no FCNT left for this run: the ticks stop
      }
    }

    void LORAListener::start_drift_test(uint32_t duration_s, uint32_t grid_ms)
    {
      if (this->parent_ == nullptr)
        return;

      // One frame per tick is ~30 ms of airtime, so the grid can be far
      // tighter than the old 17-copy burst allowed. More frames over the same
      // wall-clock time is more samples on the same baseline.
      if (grid_ms < 200)
        grid_ms = 200;

      this->drift_test_active_     = true;
      this->drift_test_duration_s_ = duration_s;
      this->drift_test_grid_ms_    = grid_ms;

      ESP_LOGI(TAG, "[%s] drift test START: %u s, hw-timed grid %u ms, plaintext single frames",
               this->get_name().c_str(), (unsigned) duration_s, (unsigned) grid_ms);

      // The START command goes out as a NORMAL 17-copy burst.
      //
      // The node is still in windowed RX at this point -- a ~29 ms window every
      // 500 ms, a 5.9% duty cycle -- which is the entire reason the burst
      // exists. A single-copy START has roughly a 6% chance of being heard, so
      // the node never enters continuous RX, never hears anything after it, and
      // the test silently does nothing. Observed exactly that: the node
      // received nothing at all for eight minutes.
      //
      // Only the TIMING frames are single copies, and by then the node is in
      // continuous RX and hears every one.
      this->build_drift_frame_(true);
      if (this->drift_frame_len_ > 0)
        this->mac0_send_(this->drift_frame_, this->drift_frame_len_, TxPolicy{}, this->drift_seal_);
      // Every tick's FCNT, reserved on the loop (HB1); see start_mode_test.
      this->reserve_fcnt_block_((duration_s * 1000) / grid_ms + 4, &this->drift_fcnts_);

      if (this->drift_timer_ == nullptr)
      {
        const esp_timer_create_args_t args = {
            .callback = &LORAListener::drift_timer_cb_,
            .arg      = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name     = "driftgrid",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &this->drift_timer_) != ESP_OK)
        {
          ESP_LOGE(TAG, "[%s] drift test: timer create failed — no ticks", this->get_name().c_str());
          this->drift_timer_ = nullptr;
          this->drift_test_active_ = false;
          return;
        }
      }
      esp_timer_stop(this->drift_timer_);

      // Give the bursted START time to land and the node time to switch into
      // continuous RX before the single-copy grid begins. One burst round is
      // 1.5 s; 3 s is comfortable margin.
      this->set_timeout("drift_grid_arm", 3000, [this, grid_ms]() {
        if (!this->drift_test_active_)
          return;
        this->build_drift_frame_(true, &this->drift_fcnts_);
        // Microsecond-resolution pacing — the entire point of this rewrite.
        esp_timer_start_periodic(this->drift_timer_, (uint64_t) grid_ms * 1000ULL);
      });

      this->set_timeout("drift_end", duration_s * 1000,
                        [this]() { this->stop_drift_test(); });
    }

    void LORAListener::stop_drift_test()
    {
      if (!this->drift_test_active_)
        return;
      this->drift_test_active_ = false;
      if (this->drift_timer_ != nullptr)
        esp_timer_stop(this->drift_timer_);
      wait_callback_idle_(this->drift_cb_busy_);   // before the OFF frame reuses drift_frame_
      this->cancel_timeout("drift_end");
      this->cancel_timeout("drift_grid_arm");

      // The OFF command is bursted like every other command: the node may
      // already have left continuous RX on its own deadline, in which case a
      // single copy would very likely be missed. Nothing needs "restoring" any
      // more — the single-copy choice never left the frame it applied to.
      this->build_drift_frame_(false);
      if (this->drift_frame_len_ > 0)
        this->mac0_send_(this->drift_frame_, this->drift_frame_len_, TxPolicy{}, this->drift_seal_);
      this->drift_frame_len_ = 0;

      ESP_LOGI(TAG, "[%s] drift test STOP", this->get_name().c_str());
    }

  }  // namespace lora_tracker
}  // namespace esphome
