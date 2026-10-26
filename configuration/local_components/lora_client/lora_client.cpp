// lora_client.cpp -- the LORAListener: ESPHome component, application,
// delivery and session (see local_components/README.md for the split).
#include "lora_client_internal.h"

namespace esphome
{
  namespace lora_tracker
  {

    void LORAListener::dump_config()
    {

      ESP_LOGCONFIG(TAG,
                    "  Short address: %d\n"
                    "  Subnet address: %d\n"
                    "  Sleep duration: %d",
                    this->short_address_, this->subnet_address_, this->sleep_duration_);
    }

    uint8_t LORAListener::s_next_login_slot_ = 0;
    onboarding::Gate LORAListener::s_onboarding_gate_;

    LORAListener::~LORAListener()
    {
      // The three esp_timer callbacks hold `this`: stop and delete them before
      // the object goes (review HB22; a real hub never destroys a listener,
      // the host harness does so constantly).
      this->mode_test_active_ = false;
      this->drift_test_active_ = false;
      this->mac_ping_active_ = false;
      for (esp_timer_handle_t *t : {&this->mode_test_timer_, &this->drift_timer_, &this->mac_ping_timer_})
      {
        if (*t == nullptr)
          continue;
        esp_timer_stop(*t);
        esp_timer_delete(*t);
        *t = nullptr;
      }
      wait_callback_idle_(this->mt_cb_busy_);
      wait_callback_idle_(this->drift_cb_busy_);
      wait_callback_idle_(this->mac_ping_cb_busy_);
      s_onboarding_gate_.release(this);
      // Tier 3: this node's keys, otherwise never freed — fine for the
      // handful of LORAListener instances a real hub constructs once at
      // boot, but the host test harness constructs many short-lived ones
      // in one process against mbedTLS's finite PSA key slot table (same
      // reasoning as CmdDispatcher's destructor on the node side). Since S2
      // the store drops the whole entry (session id and generation too): the
      // generation belongs to the listener's lifetime, as it did when it was a
      // member.
      s_keys().forget(static_cast<mac::Addr>(this->short_address_));
    }

    void LORAListener::setup()
    {
      // Claim a stagger slot in declaration order. setup() runs once per
      // listener, in YAML order, so slots are stable across reboots.
      this->login_slot_ = s_next_login_slot_++;
      // Same index, same reason: declaration order is stable across reboots.
      // Wraps at the grid depth so a 33rd client shares a slot rather than
      // addressing a slot that does not exist.
      this->grid_slot_ = (uint8_t) (this->login_slot_ % timedgrid::kSlotCount);

      // B1/B3: withdraw any grid the nodes may still be holding, BEFORE
      // anything else goes out. Our anchor is new; theirs is stale. It is a
      // BROADCAST, so the first listener (slot 0) sends it once for the whole
      // hub: one 17-copy burst per listener was ~45 s of air at 32 nodes and
      // overran the 5-buffer pool at boot (review HB12).
      if (this->login_slot_ == 0)
        this->broadcast_grid_demote();

      // The compiled YAML seed has already been applied by now (codegen emits
      // add_schedule_entry() into main.cpp's setup(), which runs before
      // App.setup()). Restoring here therefore OVERWRITES the seed with whatever
      // was last entered in Home Assistant — which is the point: without it an
      // HA-entered schedule would silently revert on every hub reboot.
      this->restore_schedule_();

      auto restore = this->restore_state_();
      if (restore.has_value())
      {
        ESP_LOGI(TAG, "[%s] NVS restore OK — version=%u rx_id=%u tx_id=%u "
                 "logged_in=%s last_sleep=%u",
                 this->get_name().c_str(),
                 (unsigned)restore->version,
                 (unsigned)restore->rx_message_id,
                 (unsigned)restore->tx_message_id,
                 restore->logged_in ? "yes" : "no",
                 (unsigned)restore->last_sleep_epoch);
        restore->apply(this);
        ESP_LOGI(TAG, "[%s] After apply — rx_id=%u tx_id=%u (tx+64) registered=%s",
                 this->get_name().c_str(),
                 (unsigned)this->seq_.rxId(),
                 (unsigned)this->seq_.txId(),
                 this->registered_ ? "yes" : "no");

        // finding 2: only an UNCHANGED config skips the REGISTER round trip a
        // hub boot would otherwise force on every node. A real YAML edit (or a
        // stub from before this field existed, config_hash == 0 by construction)
        // leaves config_synced_ false, which is exactly today's behaviour.
        const uint32_t current_hash = this->currentConfigHash_();
        if (restore->config_hash == current_hash)
        {
          this->config_synced_ = true;
          ESP_LOGI(TAG, "[%s] Config unchanged since last boot (hash 0x%08x) — "
                        "REGISTER will not be requested",
                   this->get_name().c_str(), (unsigned) current_hash);
        }
        else
        {
          ESP_LOGI(TAG, "[%s] Config hash changed (0x%08x -> 0x%08x) — "
                        "REGISTER will be requested to push it",
                   this->get_name().c_str(),
                   (unsigned) restore->config_hash, (unsigned) current_hash);
        }
      }
      else
      {
        ESP_LOGW(TAG, "[%s] NVS restore failed or version mismatch (expected v%u) — "
                 "starting fresh, waiting for REGISTER",
                 this->get_name().c_str(), (unsigned)kRestoreStateVersion);
        this->mac1_restart_session_();
        this->registered_ = false;
      }

      // A node restored as registered still holds its NVS session, but this hub
      // has no keys until its first LOGIN (delayed by the stagger, the
      // onboarding gate or the node's modelled wake). Until then commands are
      // deferred, never packed plaintext for a node that refuses plaintext
      // (review HB5); confirm_session_ clears this.
      if (this->registered_ && !this->session_confirmed_)
        this->relogin_pending_ = true;

      this->start_sleep_status_();

      // Initialise PSA Crypto (idempotent — safe from multiple instances) and
      // derive K_auth once. Every other PSA user on the hub (session key
      // derivation, burst-copy tagging) relies on this having run.
      if (!s_sec().initAuthKey())
      {
        ESP_LOGE(TAG, "psa_crypto_init or the K_auth derivation failed — LOGIN/REGISTER MICs will fail");
      }

      // Schedule a login challenge so frame counters and session keys are
      // re-negotiated after every hub reboot, respecting the node's sleep window.
      ESP_LOGI(TAG, "[%s] setup: registered=%s time=%s time_valid=%s",
               this->get_name().c_str(),
               this->registered_ ? "yes" : "no",
               this->time != nullptr ? "present" : "none",
               (this->time != nullptr && this->time->now().is_valid()) ? "yes" : "no");
      if (this->registered_)
      {
        this->login_acked_              = false;
        this->login_retry_count_        = 0;
        this->startup_login_initiated_  = false;
        if (this->time != nullptr && this->time->now().is_valid())
        {
          // Time already synced — evaluate sleep window immediately.
          this->startup_login_initiated_ = true;
          this->schedule_startup_login_();
        }
        else if (this->time != nullptr)
        {
          // NTP not yet synced — defer until we have a valid clock so the
          // sleep-window calculation is accurate.
          // IMPORTANT: guard with startup_login_initiated_, NOT login_acked_.
          // If the node is mid-wake-window after a hub reboot it will send
          // encrypted status messages that set login_acked_=true before NTP
          // syncs.  Using login_acked_ as the guard would prevent
          // schedule_startup_login_() from ever being called, leaving the
          // base-nonce map (cleared on hub reboot) unpopulated.
          // startup_login_initiated_ is only set here and is never touched by
          // incoming messages, so NTP resyncs that fire the callback a second
          // time do not disrupt an already-running session.
          ESP_LOGI(TAG, "[%s] Waiting for NTP sync before scheduling login", this->get_name().c_str());
          this->time->add_on_time_sync_callback([this]()
          {
            if (!this->startup_login_initiated_)
            {
              this->startup_login_initiated_ = true;
              this->schedule_startup_login_();
            }
          });
        }
        else
        {
          // No time component configured — fall back to a fixed delay for radio
          // initialisation + per-node stagger (no sleep-window check possible).
          static constexpr uint32_t LOGIN_STAGGER_MS = 3000;
          // Slot, not address — see the note in schedule_startup_login_().
          // This duplicate was missed when the address-based stagger was first
          // fixed; a second copy of a formula is a second chance to be wrong.
          uint32_t stagger_ms = static_cast<uint32_t>(this->login_slot_) * LOGIN_STAGGER_MS;
          uint32_t total_ms   = 5000 + stagger_ms;
          ESP_LOGW(TAG, "[%s] No time component — login in %u s (5 s radio-init + %u s stagger, no sleep-window check)",
                   this->get_name().c_str(), total_ms / 1000, stagger_ms / 1000);
          this->startup_login_initiated_ = true;
          this->set_timeout("login_startup", total_ms, [this]() { this->do_login_and_arm_retry_(); });
        }
      }
    }

    optional<LORAClientRestoreState> LORAListener::restore_state_()
    {
      this->rtc_ = this->make_entity_preference<LORAClientRestoreState>();
      LORAClientRestoreState recovered{};
      if (!this->rtc_.load(&recovered))
      {
        ESP_LOGW(TAG, "[%s] NVS key not found — no saved state", this->get_name().c_str());
        return {};
      }
      if (recovered.version != kRestoreStateVersion)
      {
        ESP_LOGW(TAG, "[%s] NVS version mismatch: stored=%u expected=%u — discarding",
                 this->get_name().c_str(),
                 (unsigned)recovered.version, (unsigned)kRestoreStateVersion);
        return {};
      }
      return recovered;
    }

    // FNV-1a-32 over the fields send_remote_config() actually puts in
    // ClientConfig. Deliberately excludes anything the NODE can change on its
    // own (battery voltage, position) — only what a YAML edit changes.
    uint32_t LORAListener::currentConfigHash_() const
    {
      uint32_t h = 2166136261u;
      auto mix = [&h](const void *data, size_t len) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
      };
      mix(&this->short_address_, sizeof(this->short_address_));
      mix(&this->subnet_address_, sizeof(this->subnet_address_));
      mix(&this->sleep_duration_, sizeof(this->sleep_duration_));
      mix(&this->battery_update_interval_, sizeof(this->battery_update_interval_));
      const std::string &name = this->get_name();
      mix(name.data(), name.size());
      return h;
    }

    void LORAListener::save_state_(bool save)
    {
      if (save)
      {
        LORAClientRestoreState restore{};
        memset(&restore, 0, sizeof(restore));
        restore.version          = kRestoreStateVersion;
        restore.rx_message_id    = this->seq_.rxId();
        restore.tx_message_id    = this->seq_.txId();
        restore.logged_in        = this->registered_;
        restore.last_sleep_epoch = this->last_sleep_epoch_;
        restore.config_hash      = this->currentConfigHash_();

        this->rtc_.save(&restore);
        ESP_LOGD(TAG, "[%s] NVS save — rx_id=%u tx_id=%u logged_in=%s last_sleep=%u",
                 this->get_name().c_str(),
                 (unsigned)restore.rx_message_id,
                 (unsigned)restore.tx_message_id,
                 restore.logged_in ? "yes" : "no",
                 (unsigned)restore.last_sleep_epoch);
      }
    }

    // ---------------------------------------------------------------------------
    // Sleep-window helpers
    // ---------------------------------------------------------------------------

    // When will this node next be listening?
    //
    // Returns 0 if the hub cannot tell, which every caller must treat as
    // "assume awake" — being wrong in that direction costs one burst, whereas
    // wrongly believing a node is asleep would stall the link.
    //
    // Two different sleeps, and using the wrong one is the bug this fixes:
    //
    //   INTERACTIVE — the hub sends CMD_SLEEP and the node sleeps for a fixed
    //       sleep_duration_. last_sleep_epoch_ + sleep_duration_ is exact.
    //
    //   AUTOMATIC — the node ignores CMD_SLEEP entirely and derives its own
    //       wake from the schedule (next event minus beacon_lead_) or from
    //       checkin_interval_, whichever comes first. sleep_duration_ does not
    //       describe it at all, so the old model reported such a node awake
    //       almost immediately and the login retry bursted into silence.
    //
    // The schedule arithmetic deliberately uses the NODE'S OWN sched::
    // next_occurrence(), vendored rather than re-implemented — a second
    // implementation would drift and the hub would transmit into silence with
    // confidence, which is the failure being fixed here.
    uint32_t LORAListener::next_wake_epoch_() const
    {
      // Reference for "when did this node last go down". last_sleep_epoch_ is
      // only set when the HUB commanded the sleep; an auto-mode node sleeps on
      // its own, so fall back to its last beacon — it sleeps within seconds of
      // sending one. Without this the whole prediction collapses to 0 and the
      // hub transmits at a sleeping node.
      const uint32_t sleep_ref = (this->last_sleep_epoch_ != 0)
                                     ? this->last_sleep_epoch_
                                     : this->last_beacon_epoch_;
      if (sleep_ref == 0)
        return 0;                       // genuinely never heard from
      if (this->time == nullptr || !this->time->now().is_valid())
        return 0;                       // no clock — cannot predict

      if (!this->auto_mode_)
        return sleep_ref + static_cast<uint32_t>(this->sleep_duration_);

      // Automatic mode: the earlier of the next scheduled event (minus the
      // beacon lead) and the periodic check-in.
      uint64_t wake = 0;

      sched::Entry entries[LORAListener::kMaxScheduleEntries];
      int count = 0;
      for (uint8_t i = 0; i < this->sched_entry_count_; i++)
      {
        sched::Entry e;
        e.minuteOfDay = this->sched_entries_[i].minute_of_day;
        e.dayMask     = this->sched_entries_[i].day_mask;
        e.action      = this->sched_entries_[i].action;
        e.positionPct = this->sched_entries_[i].position_pct;
        e.enabled     = true;           // the hub only stores entries it wants active
        entries[count++] = e;
      }

      const uint64_t now_utc = static_cast<uint64_t>(this->time->now().timestamp);
      if (count > 0)
      {
        const uint64_t next_event =
            sched::next_occurrence(entries, count, now_utc,
                                   esphome::ESPTime::timezone_offset());
        if (next_event != 0)
          wake = (next_event > this->beacon_lead_) ? (next_event - this->beacon_lead_)
                                                   : now_utc;
      }
      if (this->checkin_interval_ != 0)
      {
        const uint64_t checkin = (uint64_t) sleep_ref + this->checkin_interval_;
        if (wake == 0 || checkin < wake)
          wake = checkin;
      }
      return static_cast<uint32_t>(wake);   // 0 if neither is configured
    }

    // U-5. See the header for why both clauses are necessary and why this
    // fails open.
    bool LORAListener::beacon_overdue_() const
    {
      const uint32_t wake_at = this->next_wake_epoch_();
      if (wake_at == 0)
        return false;                   // cannot predict — never "missed"
      if (this->time == nullptr || !this->time->now().is_valid())
        return false;                   // no clock — cannot compare
      // AN INTERACTIVE NODE THAT HAS BEACONED SINCE THE HUB LAST PUT IT TO
      // SLEEP IS AWAKE, AND NOTHING PREDICTS WHEN IT SLEEPS AGAIN.
      //
      // next_wake_epoch_() predicts the wake of a sleep the hub commanded
      // (last_sleep_epoch_ + sleep_duration_). Once the node has woken and
      // beaconed, that prediction is spent: an interactive node only sleeps on
      // another CMD_SLEEP, which stamps last_sleep_epoch_ afresh. Without this
      // clause the stale prediction (or, with no sleep on record, last_beacon_ +
      // sleep_duration_) went overdue ~6 h + grace after the LAST BEACON of a
      // node that is awake, healthy and merely quiet — and an interactive node
      // beacons only on wake, boot, relogin and mode change, never while idle —
      // so it silently lost single shot (TxRefusal::BeaconMissed, a rung the
      // optimistic switch deliberately cannot bypass) and bursted forever.
      // Automatic-mode nodes are not touched: they sleep on their own schedule
      // and DO check in periodically.
      if (!this->auto_mode_ && this->last_beacon_epoch_ > this->last_sleep_epoch_)
        return false;
      const uint32_t now = static_cast<uint32_t>(this->time->now().timestamp);
      if (now < wake_at + kBeaconOverdueGraceS)
        return false;                   // not yet due, or inside the grace
      // Heard from since the predicted wake? Then it is not missed, whatever
      // the prediction says — an imminent scheduled event can pin wake_at near
      // now, and without this an awake, beaconing node reads as overdue.
      return this->last_beacon_epoch_ < wake_at;
    }

    void LORAListener::note_node_heard_()
    {
      if (this->ctl().sleep_unconfirmed)
      {
        // See on_sleep_unconfirmed_(): the node is awake (again), whichever it was.
        this->ctl().sleep_unconfirmed = false;
        if (this->op_is_sleep_())   // a later op's failure is that op's own
          this->set_command_failed_(false);
        ESP_LOGW(TAG, "[%s] Node heard again after an UNCONFIRMED sleep command — treating it as awake",
                 this->get_name().c_str());
      }
      this->onboarding_heard_ = true;
      this->last_heard_us_ = esp_timer_get_time();
      if (this->time != nullptr && this->time->now().is_valid())
        this->last_heard_epoch_ = static_cast<uint32_t>(this->time->now().timestamp);
      this->refresh_sleep_status_();   // hearing the node ends "asleep" at once
    }

    pendingshadow::NodeInputs LORAListener::pending_shadow_inputs(int64_t now_us)
    {
      pendingshadow::NodeInputs in;
      in.slot = static_cast<uint8_t>(this->grid_slot_ % timedgrid::kSlotCount);
      in.op_awaiting_ack          = this->ctl().awaiting_ack;
      in.op_deferred_until_login  = this->ctl().deferred_until_login;
      // "Login still settling" counts with an explicit relogin: both mean the
      // hub is mid-conversation with this node (TimeSync's sleepOk reads the same).
      in.relogin_pending          = this->relogin_pending_ || !this->session_confirmed_;
      // Anything the node has been sent and not confirmed, or is still owed:
      // a schedule push in flight or an edit not yet pushed, a GridSync, and
      // this boot's configuration push.
      in.push_awaiting_ack = this->push_awaiting_ack_();
      in.timesync_due = timesyncpolicy::shouldSend(
          this->auto_mode_,
          this->last_timesync_sent_us_ == 0 ? -1 : now_us - this->last_timesync_sent_us_);
      // OTA is a tracked sysop, so awaiting_ack above already covers it.
      //
      // The hub's own transmit side is global, not per node: a frame placed for
      // this node that has not fired yet, or anything draining on the air.
      in.queued_frames = this->frames_queued_(now_us);
      in.timed_mode_off = !this->timed_mode_enabled_;
      // THERE IS NO EXPLICIT LATENCY-TOLERANT FLAG. An automatic-mode node is
      // the only kind that is already unreachable between check-ins by design
      // (implementation-plan.md section 5.5), so that is the proxy.
      in.latency_tolerant = this->auto_mode_;
      // Traffic in EITHER direction: the last frame heard from it, or the last
      // downlink placed for it (that mark, once past, is the last time it was
      // addressed).
      in.since_last_traffic_us = pendingshadow::sinceLastTrafficUs(
          this->last_heard_us_, this->last_placed_t0_us_, now_us);
      return in;
    }

    // Anything the node has been sent and not confirmed, or is still owed (the
    // push_awaiting_ack input of pending_shadow_inputs).
    bool LORAListener::push_awaiting_ack_()
    {
      return this->sched_push_outstanding_() ||
             this->sched_dirty_ || this->schedule_pending() ||
             this->mac().awaiting_ack || !this->config_synced_ ||
             this->config_push_pending_;
    }

    // The hub's own transmit side is global, not per node: a frame placed for
    // this node that has not fired yet, or anything draining on the air.
    bool LORAListener::frames_queued_(int64_t now_us) const
    {
      return (this->last_placed_t0_us_ > now_us) ||
             (this->parent_ != nullptr && this->parent_->txDrainUs() > 0);
    }

    bool LORAListener::is_node_awake_() const
    {
      // (a'): HEARING THE NODE BEATS THE MODEL.
      //
      // MEASURED 2026-09-13: the hub logged "Login retry deferred 21250 s — node
      // asleep until then" moments after that very node's REGISTER. The model
      // below is purely clock-based — last recorded sleep + sleep_duration_ —
      // and nothing told it the node had just transmitted, so one lost LoginMsg
      // after any recorded sleep would stall the session for up to six hours.
      //
      // A heard uplink means "awake for THIS node's listening window", and the
      // window depends on the mode, which is the part a single rule gets wrong:
      //   * A / B (interactive, timed grid): the node keeps listening for its
      //     interactive timeout after it transmits.
      //   * C (Class A — auto mode without a grid): it listens only in RX1/RX2
      //     after its own uplink, then sleeps. Treating it as awake for half an
      //     hour would retry into a sleeping node, which is the airtime the
      //     sleep gate exists to save. RX2 closes kRx2DelayUs + one window after
      //     the uplink; 3 s is that rounded up to this model's 1 s resolution.
      //
      // Only a hearing NEWER than the last recorded sleep counts: a node heard
      // before it was told to sleep is not evidence about now.
      if (this->last_heard_epoch_ != 0 && this->last_heard_epoch_ >= this->last_sleep_epoch_ &&
          this->time != nullptr && this->time->now().is_valid())
      {
        const bool class_a =
            (this->node_mode_ == (uint32_t) NODE_MODE__MODE_AUTO) &&
            !(this->timed_mode_enabled_ && this->grid_aligned_);
        static constexpr uint32_t kClassAAwakeS = 3;
        const uint32_t window_s = class_a ? kClassAAwakeS : this->interactive_timeout_;
        const auto now = static_cast<uint32_t>(this->time->now().timestamp);
        if (now < this->last_heard_epoch_ + window_s)
          return true;
      }
      const uint32_t wake_at = this->next_wake_epoch_();
      if (wake_at == 0)
        return true;                    // cannot tell — assume awake
      return static_cast<uint32_t>(this->time->now().timestamp) >= wake_at;
    }

    uint32_t LORAListener::ms_until_node_awake_() const
    {
      if (this->is_node_awake_())
        return 0;
      auto now     = static_cast<uint32_t>(this->time->now().timestamp);
      auto wake_at = this->next_wake_epoch_();
      if (wake_at == 0 || wake_at <= now)
        return 0;
      // Convert remaining seconds to ms, capped at uint32_t max (~49 days).
      return static_cast<uint32_t>(
          std::min<uint64_t>(static_cast<uint64_t>(wake_at - now) * 1000ULL,
                             static_cast<uint64_t>(0xFFFFFFFFu)));
    }

    // THE HUB'S SLEEP MODEL (SleepModel.h documents the predicate and its edge).
    // Reuses is_node_awake_() / ms_until_node_awake_(); adds only what they do not
    // know: automatic mode, an unconfirmed sleep, "heard since the sleep", and the
    // boot margin before the modelled wake.
    sleepmodel::State LORAListener::sleep_state_() const
    {
      sleepmodel::Inputs in{};
      in.auto_mode         = this->auto_mode_;
      in.sleep_unconfirmed = this->ctl().sleep_unconfirmed;
      in.last_sleep_epoch  = this->last_sleep_epoch_;
      in.last_heard_epoch  = this->last_heard_epoch_;
      in.ms_until_awake    = this->ms_until_node_awake_();
      in.margin_ms         = sleepmodel::marginMs(this->login_slot_);
      return sleepmodel::classify(in);
    }

    // One log line, nothing sent, nothing queued, no failure raised, no relogin:
    // the user's decision is that a command issued to a sleeping node is ignored.
    // The nightly SLEEP request is not a command TO the node in that sense; it
    // keeps its own "already pending" dedupe.
    bool LORAListener::refuse_while_asleep_(const deliverypolicy::ControlCmd &c)
    {
      if (!c.is_cover && c.sysop == CLIENT_OPERATION__CMD_SLEEP)
        return false;
      if (!this->node_asleep_())
        return false;
      const int32_t tz = esphome::ESPTime::timezone_offset();
      unsigned wh = 0, wm = 0, sh = 0, sm = 0;
      sleepmodel::clockHm(this->next_wake_epoch_(), tz, &wh, &wm);
      sleepmodel::clockHm(this->last_sleep_epoch_, tz, &sh, &sm);
      ESP_LOGW(TAG, "[%s] %s ignored — node asleep until %02u:%02u (sleep acknowledged %02u:%02u)",
               this->get_name().c_str(), c.is_cover ? "cover op" : "sysop", wh, wm, sh, sm);
      return true;
    }

    void LORAListener::publish_sleep_status_(sleepmodel::State s)
    {
      const int8_t asleep = (s == sleepmodel::State::Asleep) ? 1 : 0;
      if (asleep != this->sleep_pub_asleep_)
      {
        this->sleep_pub_asleep_ = asleep;
        if (this->asleep_bsensor_ != nullptr)
          this->asleep_bsensor_->publish_state(asleep != 0);
      }
      char text[40];
      sleepmodel::statusText(s, this->next_wake_epoch_(), esphome::ESPTime::timezone_offset(),
                             text, sizeof(text));
      if (this->sleep_pub_text_ != text)
      {
        this->sleep_pub_text_ = text;
        if (this->sleep_status_tsensor_ != nullptr)
          this->sleep_status_tsensor_->publish_state(this->sleep_pub_text_);
      }
    }

    // Publishes on change only. While asleep, a timeout lands just past the moment
    // the model turns the node awake (wake minus boot margin), so the sensors go
    // OFF on time even if a Mode B node stays quiet and nothing is heard.
    void LORAListener::refresh_sleep_status_()
    {
      const sleepmodel::State s = this->sleep_state_();
      this->publish_sleep_status_(s);
      if (s != sleepmodel::State::Asleep)
      {
        this->cancel_timeout("sleep_status");
        return;
      }
      const uint32_t delay = sleepmodel::refreshDelayMs(this->ms_until_node_awake_(),
                                                        sleepmodel::marginMs(this->login_slot_));
      this->set_timeout("sleep_status", delay, [this]() { this->refresh_sleep_status_(); });
    }

    // A slow safety net on top of the event-driven refreshes: the clock may only
    // become valid (or the restored sleep become visible) after setup().
    void LORAListener::start_sleep_status_()
    {
      this->refresh_sleep_status_();
      this->set_interval("sleep_status_net", 60000, [this]() { this->refresh_sleep_status_(); });
    }



    // ---- handle_encrypted_ stages ------------------------------------------
    //
    // The crypto of each stage is LinkSecurity's (MacHdr + bytes); what stays
    // here is the order, the logging and the protobuf around it.


    // S6: demultiplexed by the one table (PortTable.h). MAC management frames
    // (ack, MAC control, ModeTest report, GridSync request) go to the MAC
    // handlers; application frames to the listener's application handlers
    // (and, in deliver_uplink_, to the children); Session frames (REGISTER,
    // LOGIN) never reach this point.
    void LORAListener::dispatch_payload_(hubframes::Up *msg)
    {
      if (porttable::uplinkPort(msg->proto_case) == mac::Port::Mac0Mgmt)
      {
        this->dispatch_mac_payload_(msg);
        return;
      }
      // A POSITION is telemetry, NOT delivery evidence (review F3, 2026-10-16,
      // docs/mac-review-2026-10-16.md). Until then any POSITION arriving while a
      // cover op awaited its ack completed that op. But the node publishes
      // POSITION at the end of EVERY motor move, button- and schedule-driven ones
      // included, and CoverPosition carries no echo of the command it would be
      // answering — so a command lost on the air while a schedule move happened
      // to end showed as delivered in Home Assistant and was never retried. The
      // rule dates from the first ack work (a657b03), before the node acked every
      // cover op; today it does (the MAC ACK, plus an AckCache re-ack of a
      // duplicate). The position itself still reaches the cover entity as an
      // application indication (deliver_uplink_).
      if (msg->proto_case == hubframes::UL_BEACON && msg->beacon)
        this->handle_beacon_(msg->beacon);
    }

    void LORAListener::deliver_uplink_(hubframes::Up *msg, const hubframes::Hdr *outer,
                                       bool authentic)
    {
      // Only an AUTHENTICATED frame may drive the MAC (ack, PhaseReport, ModeTest
      // report, grid request) or the beacon handling (reset detection, beacon
      // clock, mode, sleep model): a plaintext frame before a session exists is
      // unauthenticated, and anyone in range can forge one (review HB4 / P5).
      // Its application content still reaches the children, marked unauthentic.
      if (authentic)
        this->dispatch_payload_(msg);
      if (porttable::uplinkPort(msg->proto_case) != mac::Port::App)
        return;
      AppIndication ind;
      if (!machdr::fromHdr(outer, &ind.hdr))
        return;   // a header wider than the MAC header is never handed upward
      ind.msg       = msg;
      ind.authentic = authentic;
      for (auto *node : this->nodes_)
        node->on_app_indication(ind);
    }

    // The MAC-layer tail of dispatch_payload_ (moved out unchanged): consumed
    // here, never forwarded to a cover or a schedule.
    void LORAListener::dispatch_mac_payload_(hubframes::Up *msg)
    {
      if (msg->proto_case == hubframes::UL_ACK && msg->ack)
      {
        // §4.6's evidence, on the carrier that makes it useful. The ack answers
        // the very command single-shot is decided for, so the belief is
        // refreshed at the one moment it is about to be read — without the
        // periodic per-node uplink §4.4 prices and rejects at 32 nodes.
        this->notePhaseReport_(msg->ack->phase);
        this->handle_command_ack_(msg->ack->ack_msg_id, msg->ack->status);
      }
      // MAC-0: consumed here, never forwarded to a cover or a schedule.
      else if (msg->proto_case == hubframes::UL_MACCONTROL &&
               msg->maccontrol)
        this->handle_mac_echo_(msg->maccontrol);
      // ModeTest results. Also MAC-layer: never forwarded to a cover.
      else if (msg->proto_case == hubframes::UL_MODETESTREPORT &&
               msg->modetestreport)
      {
        this->handle_mode_test_report_(msg->modetestreport);
        this->note_warmup_refusal_();
      }
      // The node asks for its grid again. Also MAC-layer: never forwarded.
      else if (msg->proto_case == hubframes::UL_GRIDSYNCREQUEST &&
               msg->gridsyncrequest)
        this->handle_grid_sync_request_(msg->gridsyncrequest);
    }

    // A node asks for its grid again (GridSyncRequest). Advice, not a command:
    // timedmode::hubAnswersSyncRequest decides, and an answer is the ordinary
    // placed, acknowledged, re-published GridSync. Measured 2026-09-15 on node 2
    // (fw 1.0.92): a demoted node that kept its grid came back 11 ms off its
    // marks after ~17 minutes without a beacon.
    void LORAListener::handle_grid_sync_request_(const ::GridSyncRequest *r)
    {
      if (r == nullptr)
        return;
      const int64_t now_us = esp_timer_get_time();
      const uint32_t s_since_publish =
          (this->last_gridsync_publish_us_ == 0)
              ? 0xFFFFFFFFu
              : (uint32_t) ((now_us - this->last_gridsync_publish_us_) / 1000000);
      const bool answer = timedmode::hubAnswersSyncRequest(
          this->timed_mode_enabled_,
          this->parent_ != nullptr && this->parent_->gridStarted(),
          this->mac().awaiting_ack, s_since_publish);
      ESP_LOGW(TAG, "[%s] node asks for its grid (reason %u, anchor %u s old, %u samples "
                    "refused) \u2014 %s",
               this->get_name().c_str(), (unsigned) r->reason,
               (unsigned) r->ssinceanchorfix, (unsigned) r->refusedsamples,
               answer ? "publishing" : "not now");
      if (!answer)
      {
        this->gridsync_requests_ignored_++;
        return;
      }
      this->gridsync_requests_answered_++;
      this->send_grid_sync(true);
    }




    // ---- the generic delivery glue (step 3 of the unified MAC delivery refactor) ----
    //
    // One request per channel. The DECISIONS (when to retry, when to give up, which
    // ack counts) are deliverypolicy::; this only performs them. The Control channel
    // carries cover operations and sysops, the Config channel the ScheduleConfig
    // push, the Mac channel the GridSync publish.

    // ---- ScheduleConfig as a request ----

    bool LORAListener::sched_push_outstanding_() const
    {
      return deliverypolicy::ladderHasRetryLeft(this->cfg().awaiting_ack, this->cfg().retry_count,
                                                kSchedMaxRetries);
    }

    // The node acknowledged an attempt that carried `version`: that is the only
    // positive confirmation the hub gets that the schedule actually landed.
    bool LORAListener::on_schedule_acked_(uint32_t version)
    {
      ESP_LOGI(TAG, "[%s] ScheduleConfig acknowledged (version=0x%08x)",
               this->get_name().c_str(), (unsigned) version);
      // The node now holds THAT version; reflect it without waiting for its next
      // beacon, so "Schedule Pending" clears promptly in Home Assistant. If the
      // schedule has moved on since, the newer attempt is still awaited.
      this->node_sched_version_ = version;
      if (this->schedule_pending_bsensor_ != nullptr)
        this->schedule_pending_bsensor_->publish_state(this->schedule_pending());
      return version == this->schedule_version();
    }

    // The ladder is spent. The session is fine (the node is reachable by other
    // frames or it would not have been pushed to); the schedule is just not there
    // yet, and the next beacon or login pushes it again with a fresh ladder.
    void LORAListener::on_schedule_failed_()
    {
      ESP_LOGW(TAG, "[%s] ScheduleConfig not acknowledged after %u attempts — keeping the session; "
                    "the node stays on its old schedule until the next push",
               this->get_name().c_str(), (unsigned) (1 + kSchedMaxRetries));
    }

    void LORAListener::set_command_failed_(bool failed)
    {
      this->command_failed_ = failed;
      if (this->command_failed_bsensor_ != nullptr)
        this->command_failed_bsensor_->publish_state(failed);
    }

    // THE NIGHTLY SLEEP IS A TRACKED SYSOP, AND ITS EFFECTS HAPPEN ON THE ACK.
    //
    // It used to be one raw `parent_->send()`: unacked, unretried, not aligned to
    // the Mode B grid, with every hub-side consequence applied BEFORE the frame
    // was even on the air. Measured 2026-10-07 23:00: "Sleep sent" for both
    // nodes, node 1 logged in again 1.8 s later and node 2 after 3 min (no 6 h
    // sleep); and on 2026-10-10/11, with Timed Mode ON, battery updates arrived
    // all night. A promoted node listens only in its own slot, so an unaligned
    // one-shot is simply missed — and the hub had meanwhile discarded a live
    // session for a sleep that never happened.
    //
    // Now: send_tracked_sysop_(CMD_SLEEP) — the same aligned placement, adaptive
    // first-ack deadline, Rule 4 burst fallback and retransmit budget as every
    // other sysop. The hub's bookkeeping moves to on_sleep_acknowledged_().
    void LORAListener::enterSleep()
    {
      // The nightly sleep is an INTERACTIVE-mode mechanism, and applying it to
      // an auto-mode node is not merely wasted airtime — it actively breaks the
      // session (the stale login fallback of 2026-08-27 that cost a full REGISTER
      // handshake: the node's resume beacon could not be decrypted).
      // An auto-mode node derives its own wake from the schedule, so there is
      // nothing here the hub can usefully say. The node ignores the command too
      // (belt and braces), but not sending it is what protects the session.
      if (this->auto_mode_)
      {
        ESP_LOGI(TAG,
                 "Device %s is in automatic mode — skipping the nightly sleep "
                 "(it schedules its own; sending would discard the session)",
                 this->address_str());
        return;
      }

      // A second request while one is in flight would only bump the generation
      // and supersede the frame that is about to be acked.
      if (this->sleep_pending_())
      {
        ESP_LOGI(TAG, "[%s] Sleep already pending — ignoring the repeated request",
                 this->get_name().c_str());
        return;
      }

      ESP_LOGI(TAG, "Device %s entering sleep mode (tracked, effects on ack)",
               this->address_str());
      this->send_tracked_sysop_(CLIENT_OPERATION__CMD_SLEEP);
      this->arm_pending_sleep_expiry_();
    }

    bool LORAListener::op_is_sleep_() const
    {
      return this->ctl().kind == TrackedOpKind::SYSOP && this->ctl().sysop == CLIENT_OPERATION__CMD_SLEEP;
    }

    bool LORAListener::sleep_op_in_flight_() const
    {
      return this->op_is_sleep_() && (this->ctl().awaiting_ack || this->ctl().deferred_until_login);
    }

    // The node acknowledged CMD_SLEEP: it has heard it and is about to deep-sleep
    // (the node always acks, drains its queue, then sleeps). Everything the old
    // code did eagerly happens here instead.
    void LORAListener::on_sleep_acknowledged_()
    {
      this->ctl().sleep_unconfirmed = false;
      // Whatever waited behind the sleep must not be sent into a sleeping node: it
      // goes out when the node is back (confirm_session_).
      this->ctl_hold_for_wake_ = true;
      this->stamp_sleep_epoch_();
      this->refresh_sleep_status_();
      this->login_acked_         = false;
      this->login_retry_count_   = 0;
      this->pending_login_nonce_ = 0;   // sleep ends the session; mint fresh on wake
      this->cancel_timeout("login_retry");   // ("login_startup" is replaced by the arming below)
      // incrTxMessageId() no longer persists last_sleep_epoch_ as a side effect.
      this->save_state_(true);
      this->arm_post_sleep_login_fallback_();
    }

    // The sleep happened at ack time, but never EARLIER than the last uplink: the
    // ack itself is an uplink and note_node_heard_() ran just before us in the
    // same second, and is_node_awake_() reads "heard at or after the sleep" as
    // "awake again".
    void LORAListener::stamp_sleep_epoch_()
    {
      if (this->time == nullptr || !this->time->now().is_valid())
        return;
      const uint32_t now = static_cast<uint32_t>(this->time->now().timestamp);
      this->last_sleep_epoch_ = std::max(now, this->last_heard_epoch_ + 1);
    }

    // Schedule a login challenge slightly after the node is expected to wake up.
    // NODE_BOOT_MARGIN_MS gives the node time to complete its boot sequence and
    // send REGISTER, which cancels this timer (REGISTER-path is the primary path).
    // This timer is only the fallback for lost REGISTER packets.
    // LOGIN_STAGGER_MS per SLOT prevents simultaneous login transmissions when
    // multiple nodes wake from sleep at roughly the same time. Slot, not
    // address — this is the site that produced the "51 s stagger" and
    // "54 s stagger" seen in the hub log for addresses 17 and 18.
    void LORAListener::arm_post_sleep_login_fallback_()
    {
      static constexpr uint32_t NODE_BOOT_MARGIN_MS = 5000;
      static constexpr uint32_t LOGIN_STAGGER_MS    = 3000;
      uint32_t stagger_ms = static_cast<uint32_t>(this->login_slot_) * LOGIN_STAGGER_MS;
      uint32_t wake_ms = static_cast<uint32_t>(
          std::min<uint64_t>(this->sleep_duration_ * 1000ULL,
                             static_cast<uint64_t>(0xFFFFFFFFu)));
      uint32_t total_ms = wake_ms + NODE_BOOT_MARGIN_MS + stagger_ms;
      ESP_LOGI(TAG, "[%s] Sleep acknowledged — login fallback timer in %u s (%u s sleep + %u s boot margin + %u s stagger)",
               this->get_name().c_str(), total_ms / 1000,
               wake_ms / 1000, NODE_BOOT_MARGIN_MS / 1000, stagger_ms / 1000);
      this->fallback_rechecked_ = false;
      this->set_timeout("login_startup", total_ms, [this]() { this->post_sleep_fallback_fire_(); });
    }

    // The fallback is for a wake whose REGISTER was lost. If the node instead
    // resumed its session (a decrypted uplink confirmed it) a second LOGIN and the
    // GridSync it triggers are pure waste - but only once the node has shown it
    // hears us (acked something since the confirmation), because a resumed node
    // that drops our downlinks is exactly what a fresh LOGIN repairs.
    void LORAListener::post_sleep_fallback_fire_()
    {
      if (this->login_acked_ && this->session_confirmed_)
      {
        if (this->acked_since_confirm_)
        {
          ESP_LOGI(TAG, "[%s] Post-sleep login fallback not needed: the node resumed its session and acks",
                   this->get_name().c_str());
          return;
        }
        if (!this->fallback_rechecked_)
        {
          this->fallback_rechecked_ = true;
          this->set_timeout("login_startup", modebsup::kFallbackRecheckMs,
                            [this]() { this->post_sleep_fallback_fire_(); });
          return;
        }
      }
      this->do_login_and_arm_retry_();
    }

    // Every retry was spent and the node never acked CMD_SLEEP. Unlike any other
    // tracked op this does NOT mean the session is dead, and tearing it down
    // ("Command failed after retries — clearing session, forcing re-login") would
    // be wrong either way:
    //   * the node may have heard it, acked (ack lost on the air) and gone to
    //     sleep — then a forced re-login transmits into a sleeping node;
    //   * or it never heard it and is awake and well — then the session it holds
    //     is perfectly good and discarding it only costs a LOGIN/REGISTER cycle.
    // The hub cannot tell which. So: record NOTHING as if the sleep had happened
    // (last_sleep_epoch_, login_acked_, the login fallback are untouched — the
    // model keeps saying "awake", the safe error), keep the session, shout, and
    // let the next thing heard from the node settle it (note_node_heard_): a node
    // that did sleep announces itself with a REGISTER at its wake, which the
    // normal path handles; one that did not is simply heard.
    void LORAListener::on_sleep_unconfirmed_()
    {
      this->ctl().awaiting_ack   = false;
      this->ctl().sleep_unconfirmed = true;
      this->refresh_sleep_status_();
      this->set_command_failed_(true);
      ESP_LOGE(TAG, "[%s] SLEEP command UNCONFIRMED after %u retries — the node may have "
                    "slept (ack lost) or never heard it. Session KEPT, sleep NOT recorded; "
                    "the next uplink from the node settles it",
               this->get_name().c_str(), (unsigned) kOpMaxRetries);
    }

    // A sleep stuck behind a relogin (the session never came back: the node is
    // absent) would otherwise fire whenever the session finally confirms —
    // possibly days later, in the middle of the afternoon.
    void LORAListener::arm_pending_sleep_expiry_()
    {
      this->set_timeout("sleep_pending_expiry", kPendingSleepExpiryMs,
                        [this]() { this->expire_pending_sleep_(); });
    }

    void LORAListener::expire_pending_sleep_()
    {
      // Actively retrying with a live session: its own retry budget ends it.
      // (relogin_pending_ is cleared only by confirm_session_(), which also sends
      // a deferred op, so it alone says "still waiting for a session".)
      if (!this->relogin_pending_ || !this->drop_stale_sleep_())
        return;
      ESP_LOGW(TAG, "[%s] Sleep still waiting for a session after %u s — dropped, not sent late",
               this->get_name().c_str(), (unsigned) (kPendingSleepExpiryMs / 1000));
    }

    // The sleep in flight (or deferred) is given up as unconfirmed and the command
    // behind it, if any, goes next; a sleep still waiting in line is just removed
    // (it was never sent: nothing is unconfirmed). False: there was no sleep.
    bool LORAListener::drop_stale_sleep_()
    {
      if (this->sleep_op_in_flight_())
      {
        this->ctl().deferred_until_login = false;
        this->cancel_timeout(dlv_timer_(CH_CONTROL));
        this->on_sleep_unconfirmed_();
        this->ctl_next_();
        return true;
      }
      return this->ctl_queue_.removeSysop(CLIENT_OPERATION__CMD_SLEEP);
    }

    bool LORAListener::sleep_pending_() const
    {
      return this->sleep_op_in_flight_() || this->ctl_queue_.hasSysop(CLIENT_OPERATION__CMD_SLEEP);
    }

    void LORAListener::triggerOTA()
    {
      ESP_LOGI(TAG, "Device %s triggering OTA", this->address_str());
      // P1: OTA is a one-shot sysop — send it through the tracked/acked path so a
      // dropped or replay-rejected frame is retransmitted until the node acks,
      // instead of being silently lost (the observed post-reboot OTA drop).  The
      // sysop is sealed in-session by the delivery service, so it cannot be forged.
      this->send_tracked_sysop_(CLIENT_OPERATION__CMD_OTA);
    }

    void LORAListener::sendDebugSysop(int32_t op)
    {
      ESP_LOGI(TAG, "Device %s: debug sysop %d", this->address_str(), (int) op);
      // Same tracked/acked path as every other sysop -- a dropped toggle
      // recovers via the hub's own retransmit rather than silently not
      // taking effect. See DebugFlags.h (node repo) for what each op does.
      this->send_tracked_sysop_(op);
    }



    // ---- S4: the single MAC data path ---------------------------------------
    //
    // Every hub->node data frame is framed by mac_frame_ (header, msgid, pack,
    // seal) and leaves through mac0_send_, which attaches the frame's own copy
    // sealer. LOGIN and the GridDemote broadcast build their own bootstrap
    // frames (MIC-authenticated / unsealed broadcast) but leave through
    // mac0_send_ too.

    bool LORAListener::mac_frame_(hubframes::Down *op, MacFrame *out)
    {
      return this->mac_frame_(op, this->incrTxMessageId(), this->session_confirmed_, out);
    }

    bool LORAListener::mac_frame_(hubframes::Down *op, uint32_t msgid, bool encrypt,
                                  MacFrame *out)
    {
      return this->v1_frame_(op, msgid, encrypt, out);
    }

    // ---- S8: MAC wire v1 framing (docs/mac-wire-format.md) -------------------


    // ------------------------------------------------------------------------
    // P4: schedule push
    // ------------------------------------------------------------------------
    // ---- Home Assistant schedule editing ----------------------------------
    //
    // Slot-addressed rather than append-only: HA edits one field of one slot,
    // so the entity needs a stable index. add_schedule_entry() below keeps its
    // append semantics for the YAML seed.

    void LORAListener::set_slot_minute(uint8_t slot, uint16_t minute_of_day)
    {
      if (slot >= kMaxScheduleEntries) return;
      this->sched_entries_[slot].minute_of_day = minute_of_day;
      if (slot >= this->sched_entry_count_) this->sched_entry_count_ = slot + 1;
      this->sched_dirty_ = true;
      this->save_schedule_();
    }

    void LORAListener::set_slot_days(uint8_t slot, uint8_t day_mask)
    {
      if (slot >= kMaxScheduleEntries) return;
      this->sched_entries_[slot].day_mask = day_mask;
      if (slot >= this->sched_entry_count_) this->sched_entry_count_ = slot + 1;
      this->sched_dirty_ = true;
      this->save_schedule_();
    }

    void LORAListener::set_slot_action(uint8_t slot, uint8_t action)
    {
      if (slot >= kMaxScheduleEntries) return;
      this->sched_entries_[slot].action = action;
      if (slot >= this->sched_entry_count_) this->sched_entry_count_ = slot + 1;
      this->sched_dirty_ = true;
      this->save_schedule_();
    }

    void LORAListener::set_slot_position(uint8_t slot, uint8_t position_pct)
    {
      if (slot >= kMaxScheduleEntries) return;
      this->sched_entries_[slot].position_pct = position_pct;
      this->sched_dirty_ = true;
      this->save_schedule_();
    }

    void LORAListener::set_slot_enabled(uint8_t slot, bool on)
    {
      if (slot >= kMaxScheduleEntries) return;
      this->sched_entries_[slot].enabled = on;
      if (slot >= this->sched_entry_count_) this->sched_entry_count_ = slot + 1;
      this->sched_dirty_ = true;
      this->save_schedule_();
    }

    // Persist the whole slot table.
    //
    // Without this an HA-entered schedule would be silently reverted to the
    // compiled YAML on the next hub reboot — the same failure the Auto Mode
    // switch already had, and with no error anywhere to explain it.
    // ---- compact text form -------------------------------------------------

    bool LORAListener::set_schedule_text(const char *text, char *err, size_t err_len)
    {
      const auto r = scheduletext::parse(text);
      if (!r.ok)
      {
        if (err != nullptr && err_len > 0)
        {
          size_t i = 0;
          for (; r.error[i] != 0 && i < err_len - 1; i++) err[i] = r.error[i];
          err[i] = 0;
        }
        ESP_LOGW(TAG, "[%s] Schedule text rejected: %s — schedule unchanged",
                 this->get_name().c_str(), r.error);
        return false;   // parse() guarantees r.count == 0 here
      }

      // Whole-table replace. Slots beyond the new count are cleared rather than
      // left behind, or removing an entry from the text would leave it running.
      for (uint8_t i = 0; i < kMaxScheduleEntries; i++)
      {
        if (i < r.count)
        {
          this->sched_entries_[i].minute_of_day = r.entries[i].minuteOfDay;
          this->sched_entries_[i].day_mask      = r.entries[i].dayMask;
          this->sched_entries_[i].action        = r.entries[i].action;
          this->sched_entries_[i].position_pct  = r.entries[i].positionPct;
          this->sched_entries_[i].enabled       = true;
        }
        else
        {
          this->sched_entries_[i] = SchedEntryCfg{};
        }
      }
      this->sched_entry_count_ = r.count;
      this->sched_dirty_       = true;
      this->save_schedule_();
      if (err != nullptr && err_len > 0) err[0] = 0;
      ESP_LOGI(TAG, "[%s] Schedule set from text: %u entries — will push at next beacon",
               this->get_name().c_str(), (unsigned) r.count);
      return true;
    }

    void LORAListener::schedule_text(char *out, size_t out_len) const
    {
      scheduletext::Entry e[kMaxScheduleEntries];
      uint8_t n = 0;
      for (uint8_t i = 0; i < this->sched_entry_count_; i++)
      {
        if (!this->sched_entries_[i].enabled) continue;
        e[n].minuteOfDay = this->sched_entries_[i].minute_of_day;
        e[n].dayMask     = this->sched_entries_[i].day_mask;
        e[n].action      = this->sched_entries_[i].action;
        e[n].positionPct = this->sched_entries_[i].position_pct;
        n++;
      }
      scheduletext::format(e, n, out, out_len);
    }

    void LORAListener::save_schedule_()
    {
      SchedPersist blob{};
      blob.version   = kSchedPersistVersion;
      blob.count     = this->sched_entry_count_;
      blob.auto_mode = this->auto_mode_;
      for (uint8_t i = 0; i < kMaxScheduleEntries; i++)
        blob.entries[i] = this->sched_entries_[i];
      if (!this->sched_pref_.save(&blob))
        ESP_LOGW(TAG, "[%s] Could not persist schedule", this->get_name().c_str());
    }

    void LORAListener::restore_schedule_()
    {
      // Discriminated from the LORAClientRestoreState preference on the same
      // entity — without the version argument both would key to one slot.
      this->sched_pref_ = this->make_entity_preference<SchedPersist>(kSchedPersistVersion);
      SchedPersist blob{};
      if (!this->sched_pref_.load(&blob) || blob.version != kSchedPersistVersion)
      {
        ESP_LOGI(TAG, "[%s] No stored schedule — using the YAML seed",
                 this->get_name().c_str());
        return;
      }
      this->sched_entry_count_ = (blob.count <= kMaxScheduleEntries) ? blob.count
                                                                    : kMaxScheduleEntries;
      for (uint8_t i = 0; i < kMaxScheduleEntries; i++)
        this->sched_entries_[i] = blob.entries[i];

      // Assigned directly, NOT via set_auto_mode(): that would try to send a
      // mode sysop, and at boot there is no session to send it on. The push
      // happens anyway — sched_dirty_ below moves the version, so the node
      // picks the mode up with the schedule at its next beacon.
      this->auto_mode_ = blob.auto_mode;
      this->sched_restored_ = true;
      this->sched_dirty_    = true;
      ESP_LOGI(TAG, "[%s] Restored %u schedule slots + mode=%s from flash (YAML seed ignored)",
               this->get_name().c_str(), (unsigned) this->sched_entry_count_,
               this->auto_mode_ ? "AUTO" : "INTERACTIVE");
    }

    void LORAListener::add_schedule_entry(uint16_t minute_of_day, uint8_t day_mask,
                                          uint8_t action, uint8_t position_pct)
    {
      if (this->sched_entry_count_ >= kMaxScheduleEntries)
      {
        ESP_LOGE(TAG, "[%s] More than %u schedule entries — extra ones ignored",
                 this->get_name().c_str(), (unsigned) kMaxScheduleEntries);
        return;
      }
      // First-boot seed only.
      //
      // ORDER: codegen emits these calls into main.cpp's setup(), which runs
      // BEFORE App.setup() and therefore before restore_schedule_(). So the seed
      // lands first and a persisted schedule then overwrites it — that is what
      // makes stored values win, not this guard. The guard only covers a
      // hypothetical runtime caller after restore has run.
      if (this->sched_restored_)
        return;
      SchedEntryCfg &e = this->sched_entries_[this->sched_entry_count_++];
      e.minute_of_day  = minute_of_day;
      e.day_mask       = day_mask;
      e.action         = action;
      e.position_pct   = position_pct;
      e.enabled        = true;   // YAML only lists entries it wants active
      this->sched_dirty_ = true;
    }

    void LORAListener::set_auto_mode(bool on)
    {
      if (this->auto_mode_ == on)
        return;
      // Asked BEFORE the flip: the flip itself would make an interactive node read
      // as "auto" (never gated). A sleeping node is not told now; the schedule push
      // at its wake carries the mode.
      const bool ignored = this->session_confirmed_ &&
          this->refuse_while_asleep_({false, 0, 0, 0.0f, on ? CLIENT_OPERATION__CMD_MODE_AUTO
                                                            : CLIENT_OPERATION__CMD_MODE_INTERACTIVE});
      this->auto_mode_   = on;
      this->sched_dirty_ = true;
      // Persist the intent. Without this a hub reboot reverted the mode to the
      // compiled YAML default with no error anywhere — observed on node 1,
      // which was silently put back into AUTO and was therefore asleep and
      // unreachable when an OTA was triggered at it.
      //
      // set_auto_mode_default() (the YAML seed) deliberately does NOT persist:
      // it writes auto_mode_ directly and runs before setup(), so it seeds and
      // restore_schedule_() then overrides it.
      this->save_schedule_();
      ESP_LOGI(TAG, "[%s] Mode set to %s — will push at next beacon",
               this->get_name().c_str(), on ? "AUTO" : "INTERACTIVE");
      // An awake node can be switched immediately; a sleeping one picks it up
      // from the schedule push when it next beacons.
      if (this->session_confirmed_ && !ignored)
        this->send_tracked_sysop_(on ? CLIENT_OPERATION__CMD_MODE_AUTO
                                     : CLIENT_OPERATION__CMD_MODE_INTERACTIVE);
    }

    uint32_t LORAListener::schedule_version()
    {
      if (!this->sched_dirty_)
        return this->sched_version_;

      // CRC32 over the canonical blob. Anything the node would act on
      // differently must feed in here, or a change would not be pushed.
      uint32_t crc = 0xFFFFFFFFu;
      auto feed = [&crc](uint32_t v) {
        for (int b = 0; b < 4; b++)
        {
          crc ^= (v >> (b * 8)) & 0xFFu;
          for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
        }
      };

      feed(this->auto_mode_ ? 1u : 0u);
      feed(this->interactive_timeout_);
      feed(this->checkin_interval_);
      feed(this->beacon_lead_);
      feed(this->post_event_window_);
      feed(this->catchup_window_);
      feed(this->sched_entry_count_);
      for (uint8_t i = 0; i < this->sched_entry_count_; i++)
      {
        const SchedEntryCfg &e = this->sched_entries_[i];
        // `enabled` MUST feed the CRC: switching a slot off changes what the
        // node should act on, and a version that did not move would leave the
        // node running the old schedule with no indication anything had changed.
        feed(e.enabled ? 1u : 0u);
        feed(e.minute_of_day);
        feed(e.day_mask);
        feed(e.action);
        feed(e.position_pct);
      }

      this->sched_version_ = crc ^ 0xFFFFFFFFu;
      // Version 0 is the node's "I have no schedule" sentinel, so never mint it
      // — a real schedule that happened to hash to 0 would look unconfigured
      // and be re-pushed on every single beacon.
      if (this->sched_version_ == 0)
        this->sched_version_ = 1;
      this->sched_dirty_ = false;
      return this->sched_version_;
    }

    bool LORAListener::schedule_pending()
    {
      return this->node_sched_version_ != this->schedule_version();
    }

    void LORAListener::send_schedule_config()
    {
      if (this->parent_ == nullptr)
        return;
      // See relogin_pending_. Checked before the msgid is taken, so a deferred
      // push does not burn one; confirm_session_ pushes the schedule again.
      if (this->relogin_pending_)
      {
        ESP_LOGD(TAG, "[%s] ScheduleConfig deferred — session being rebuilt", this->get_name().c_str());
        return;
      }

      DeliveryRequest req;
      req.what     = "ScheduleConfig";
      req.policy   = deliverypolicy::kSchedulePolicy;
      req.encode   = [this](bool retry, uint32_t *version, const DeliveryRequest::FrameFn &frame) {
        return this->encode_schedule_(retry, version, frame);
      };
      req.place        = [this](const MacFrame &f, const DeliveryAttempt &a) { this->place_schedule_(f, a); };
      req.on_delivered = [this](const DeliveryConfirm &c) { return this->on_schedule_acked_(c.tag); };
      req.on_fail  = [this]() { this->on_schedule_failed_(); };
      this->deliver_(CH_CONFIG, std::move(req));
    }

    // ONE attempt at the push, built from the schedule as it is NOW (a resend
    // carries the latest version). Its tag is the version it carries.
    bool LORAListener::encode_schedule_(bool retry, uint32_t *version,
                                        const DeliveryRequest::FrameFn &frame)
    {
      (void) retry;
      hubframes::Down op_message {};
      // Single-shot, NOT bursted: a full 8-entry schedule is ~152 B, whose
      // ~95 ms airtime overruns the 88 ms burst slot. It is only ever sent in
      // reply to a beacon, so the node is provably listening and the burst
      // would be redundant anyway.

      ScheduleEntry  entries[kMaxScheduleEntries];
      ScheduleEntry *entry_ptrs[kMaxScheduleEntries];
      // Only ENABLED slots go on the wire, compacted. The node treats every
      // entry it receives as active (it has no enabled flag of its own), and a
      // disabled slot must not cost airtime — each entry is ~7 B in a frame
      // whose 17-copy burst already fills half its round.
      uint8_t n = 0;
      for (uint8_t i = 0; i < this->sched_entry_count_; i++)
      {
        if (!this->sched_entries_[i].enabled)
          continue;
        schedule_entry__init(&entries[n]);
        entries[n].minuteofday = this->sched_entries_[i].minute_of_day;
        entries[n].daymask     = this->sched_entries_[i].day_mask;
        entries[n].action      = static_cast<SchedAction>(this->sched_entries_[i].action);
        entries[n].positionpct = this->sched_entries_[i].position_pct;
        entry_ptrs[n]          = &entries[n];
        n++;
      }

      ScheduleConfig sc = SCHEDULE_CONFIG__INIT;
      sc.version              = this->schedule_version();
      sc.mode                 = this->auto_mode_ ? NODE_MODE__MODE_AUTO
                                                 : NODE_MODE__MODE_INTERACTIVE;
      sc.interactivetimeout_s = this->interactive_timeout_;
      sc.checkininterval_s    = this->checkin_interval_;
      sc.beaconlead_s         = this->beacon_lead_;
      sc.posteventwindow_s    = this->post_event_window_;
      sc.catchupwindow_s      = this->catchup_window_;
      sc.n_entries            = n;   // enabled slots only, not the table size
      sc.entries              = n ? entry_ptrs : nullptr;

      op_message.cmd_case = hubframes::DL_SCHEDULE;
      op_message.schedule = &sc;

      *version = sc.version;
      if (frame(&op_message))
        return true;
      ESP_LOGE(TAG, "[%s] Failed to pack ScheduleConfig", this->get_name().c_str());
      return false;
    }

    void LORAListener::place_schedule_(const MacFrame &f, const DeliveryAttempt &a)
    {
      {
        // PLACED. It used to go out through the bare send(), so in Mode B it
        // left whenever the queue drained — into a window the node had stopped
        // opening. That made the largest frame on the link (152 B) also the one
        // least likely to be heard.
        //
        // Single-shot ELIGIBLE, deliberately, and it is the only one of the
        // three routine downlinks that is: the node ACKS a schedule push, so a
        // missed single copy is visible and the retry above is already a burst.
        // That is exactly Rule 4's bounded exposure. TimeSync carries no ack
        // and asks for the burst explicitly.
        this->send_aligned_(f.bytes.data(), f.bytes.size(), f.seal);
        // The retransmit is the delivery glue's: the node acks a schedule push
        // (unlike TimeSync), and that ack is what ends the ladder.
        ESP_LOGI(TAG, "[%s] ScheduleConfig %s (version=0x%08x mode=%s entries=%u %u B msgid=%u)",
                 this->get_name().c_str(), a.retry ? "resent" : "sent", (unsigned) a.tag,
                 this->auto_mode_ ? "AUTO" : "INTERACTIVE",
                 (unsigned) this->sched_entry_count_, (unsigned) f.bytes.size(),
                 (unsigned) f.msgid);
      }
    }

    // P2: a node announced a wake.  Everything here is observation — the beacon
    // does not (yet) change what the hub sends, so shipping it ahead of the
    // scheduler is behaviour-neutral apart from the clock-offset sensor.
    // esp_reset_reason_t as a name. The node sends the raw IDF value so the hub
// does not have to track an enum the IDF owns; this is purely for the log.
//
// It matters because a field node has no serial cable, and "why did it boot"
// has had to be GUESSED three separate times: a cold boot that lost RTC RAM, a
// run of OTA restarts, and a node that stopped answering entirely. POWERON on a
// node that should have deep-slept means something reset it; BROWNOUT on a node
// with a low pack means the pack.
static const char *s_reset_reason_name(uint32_t r)
{
  switch (r)
  {
  case 1:  return "POWERON";
  case 2:  return "EXT";
  case 3:  return "SW";
  case 4:  return "PANIC";
  case 5:  return "INT_WDT";
  case 6:  return "TASK_WDT";
  case 7:  return "WDT";
  case 8:  return "DEEPSLEEP";
  case 9:  return "BROWNOUT";
  case 10: return "SDIO";
  default: return "?";
  }
}

// POSITIONAL: the index IS the wire value, so a new WakeReason must be
// appended here in the same commit or it reads as "?" — the bound below
// is the other half and they must move together.
static const char *s_wake_reason_name(int r)
{
  static const char *const kReason[] = {"BOOT", "TIMER_EVENT", "TIMER_CHECKIN",
                                        "BUTTON", "UNKNOWN", "MODE_CHANGED"};
  return (r >= 0 && r < (int) (sizeof(kReason) / sizeof(kReason[0]))) ? kReason[r] : "?";
}

void LORAListener::handle_beacon_(const ::NodeWakeBeacon *b)
    {
      if (b == nullptr)
        return;

      this->last_beacon_reason_  = b->reason;
      this->node_mode_           = b->mode;
      this->node_sched_version_  = b->schedversion;
      this->node_fw_version_     = b->fwversion;
      this->node_session_resume_ = b->sessionresume;

      this->note_wake_clock_(b);
      this->note_class_a_ledger_(b);
      const bool reset_changed = this->note_node_reset_(b);

      // §4.6's promotion evidence. handle_beacon_ runs only for a DECRYPTED
      // beacon, which is what makes this an authenticated observation.
      this->notePhaseReport_(b->phase);

      // The node is back from a reboot that cost it its RAM grid, on a session it
      // RESUMED from NVS (so no LOGIN, hence no D1 publish). After notePhaseReport_,
      // which would otherwise overwrite the verdict with this very beacon's report.
      this->regrid_after_reboot_(b, reset_changed);

      this->note_beacon_clock_(b);

      // P2b: ALWAYS answer a beacon with a TimeSync.
      //
      // Two jobs in one frame.  It refreshes the node's clock on every wake
      // (correcting whatever drift accumulated while it slept), and — because
      // it is encrypted — it is the node's proof that the resumed session
      // actually works.  A node that took the resume path is sitting on a
      // fallback timer right now: if this reply never decrypts, it re-registers.
      //
      // Same timeout name as the login path, so the two can never double-send:
      // set_timeout replaces an existing timeout of the same name.
      this->set_timeout("timesync_push", 750, [this]() { this->send_timesync(); });

      this->sync_auto_mode_switch_(b);

      if (this->schedule_pending_bsensor_ != nullptr)
        this->schedule_pending_bsensor_->publish_state(this->schedule_pending());

      // P4: push the schedule if the node is not already on our version.
      //
      // Version comparison rather than a dirty flag, so this is self-healing:
      // a node that missed a push, was reflashed, or lost its config.txt
      // reports a stale version and gets corrected on its very next wake,
      // without the hub needing to have remembered that it owed one.
      //
      // Deferred past the TimeSync so the two do not land on top of each other,
      // and ordered second because the node needs a clock before a schedule is
      // of any use to it.
      if (this->schedule_pending())
      {
        ESP_LOGI(TAG, "[%s] Schedule pending (node=0x%08x hub=0x%08x) — pushing",
                 this->get_name().c_str(), (unsigned) this->node_sched_version_,
                 (unsigned) this->schedule_version());
        this->set_timeout("schedule_push", 2000, [this]() { this->send_schedule_config(); });
      }
    }

    void LORAListener::note_wake_clock_(const ::NodeWakeBeacon *b)
    {
      // Mode C, MAC-0: the wake clock. This beacon carries the RTC tick count of
      // the PREVIOUS one; pair it first, then file this beacon's own stamp for
      // the next. The stamp is used only when it belongs to this frame: a
      // plaintext frame on a confirmed session does not refresh
      // last_uplink_t0_us_, and pairing a stale stamp would bias the rate.
      if (this->parent_ != nullptr && this->last_uplink_t0_us_ > 0 &&
          this->last_uplink_t0_us_ == this->parent_->last_rx_t0_us())
      {
        const bool sampled = this->wake_fit_.addReported(b->prevbeaconfcnt, b->prevbeacont0ticks);
        this->wake_fit_.noteHeard(this->last_uplink_msgid_, this->last_uplink_t0_us_);
        if (sampled)
          ESP_LOGW(TAG, "[%s] Wake clock: n %u span %u s ppm %d | crystal period %u Q19 -> %d ppm "
                        "| prev wake: windows %u hits %u detected %u crcValid %u",
                   this->get_name().c_str(), (unsigned) this->wake_fit_.n,
                   (unsigned) this->wake_fit_.spanS(), (int) this->wake_fit_.ppm(),
                   (unsigned) b->rtcperiodq19, (int) wakeclock::crystalErrorPpm(b->rtcperiodq19),
                   (unsigned) b->prevwakewindows, (unsigned) b->prevwakehits,
                   (unsigned) b->prevwakedetected, (unsigned) b->prevwakecrcvalid);

        // Mode C's pass line: how far off the wake was against what the node
        // wanted, through this fit. Needs a rate (two samples) and a node that
        // reports its sleep request.
        if (sampled && this->wake_fit_.ready() &&
            b->prevsleeprequestedus != 0 && b->prevsleepappliedus != 0)
          ESP_LOGW(TAG, "[%s] Wake timing: requested %llu us, applied %llu us (%s) -> "
                        "error %d ppm against counter %d ppm",
                   this->get_name().c_str(), (unsigned long long) b->prevsleeprequestedus,
                   (unsigned long long) b->prevsleepappliedus,
                   b->prevsleepappliedus == b->prevsleeprequestedus ? "uncorrected" : "corrected",
                   (int) wakeclock::wakeTimingErrorPpm(b->prevsleeprequestedus,
                                                       b->prevsleepappliedus,
                                                       this->wake_fit_.ppm()),
                   (int) this->wake_fit_.ppm());
      }
    }

    void LORAListener::note_class_a_ledger_(const ::NodeWakeBeacon *b)
    {
      // Mode C's ledger, stages 1-3: what the node's PREVIOUS wake actually
      // saw. Every beacon carries it, so this is the other half of the
      // offered count above and the pair is what makes a Class A success rate
      // a number rather than a correlation exercise.
      //
      // IDEMPOTENT ON prevbeaconfcnt. Each beacon reports the previous wake,
      // so two beacons from one wake would add that wake twice — precisely
      // what fw 1.1.2 and 1.1.3 did before the one-uplink fix. Keying on the
      // msgid means a duplicate, a retransmission or a regression of that fix
      // cannot inflate the ledger.
      if (b->prevbeaconfcnt != 0 &&
          b->prevbeaconfcnt != this->class_a_last_prev_msgid_)
      {
        this->class_a_last_prev_msgid_ = b->prevbeaconfcnt;
        this->class_a_stats_.windows   += b->prevwakewindows;
        this->class_a_stats_.hits      += b->prevwakehits;
        this->class_a_stats_.detected  += b->prevwakedetected;
        this->class_a_stats_.crc_valid += b->prevwakecrcvalid;
      }
    }

    bool LORAListener::note_node_reset_(const ::NodeWakeBeacon *b)
    {
      // N-3 (hub side): a non-deep-sleep reset means the node's SessionManager
      // restored rx_id_ with a +16 margin and a fully-seen bitmap over it (see
      // BlindsESP's SessionManager::load()) — the node will refuse to accept
      // any downlink msgid inside that margin. Advance this listener's own
      // downlink counter to match, once per actual reboot (not once per
      // beacon — DEEPSLEEP wakes report the same reason on every beacon).
      const bool changed = (b->resetreason != this->last_handled_reset_reason_);
      this->last_handled_reset_reason_ = b->resetreason;
      if (modebsup::bootAdvancesTx((uint32_t) b->reason, b->resetreason, changed, b->sessionresume,
                                   b->prevbeaconfcnt != this->last_wake_key_,
                                   this->s_since_(this->last_boot_advance_us_)))
        this->advance_tx_for_boot_(b);
      return changed;
    }

    uint32_t LORAListener::s_since_(int64_t t_us) const
    {
      return (t_us == 0) ? 0xFFFFFFFFu : (uint32_t) ((esp_timer_get_time() - t_us) / 1000000);
    }

    // Once per node boot (see modebsup::bootAdvancesTx): the node restored its
    // replay mark kRxPersistMargin ahead of what it had persisted.
    void LORAListener::advance_tx_for_boot_(const ::NodeWakeBeacon *b)
    {
      const int64_t now_us = esp_timer_get_time();
      this->last_boot_advance_us_ = (now_us > 0) ? now_us : 1;
      this->last_wake_key_        = b->prevbeaconfcnt;
      this->mac1_skip_tx_(modebsup::kTxBootMargin);
      ESP_LOGI(TAG, "[%s] Node reset (%s%s) — advancing tx_message_id by %u to %u",
               this->get_name().c_str(), s_reset_reason_name(b->resetreason),
               b->resetreason == modebsup::kResetDeepSleep ? ", resumed wake" : ", non-deep-sleep",
               (unsigned) modebsup::kTxBootMargin, (unsigned) this->seq_.txId());
    }

    void LORAListener::note_beacon_clock_(const ::NodeWakeBeacon *b)
    {
      const char *reason = s_wake_reason_name(b->reason);
      // Reference point for predicting the next check-in — see
      // last_beacon_epoch_. The node beacons on every wake, so this is the
      // most recent moment it was provably awake.
      if (this->time != nullptr && this->time->now().is_valid())
        this->last_beacon_epoch_ = static_cast<uint32_t>(this->time->now().timestamp);

      // Clock drift: the whole reason TimeSync shipped a phase early.  The node
      // stamps its own epoch into the beacon; comparing against ours turns drift
      // into a Home Assistant number instead of something only a serial cable
      // can see.  Only meaningful when BOTH clocks are valid.
      if (b->clockvalid && b->nodeepoch != 0 &&
          this->time != nullptr && this->time->now().is_valid())
      {
        const int64_t hub_epoch = static_cast<int64_t>(this->time->now().timestamp);
        const int64_t offset_s  = static_cast<int64_t>(b->nodeepoch) - hub_epoch;
        this->clock_offset_valid_ = true;
        this->clock_offset_s_     = static_cast<int32_t>(offset_s);
ESP_LOGI(TAG, "[%s] Beacon: reason=%s reset=%s clock_offset=%+lld s fw=%u resume=%d v=%.2f pos=%.2f",
                 this->get_name().c_str(), reason,
                 s_reset_reason_name(b->resetreason), (long long) offset_s,
                 (unsigned) b->fwversion, (int) b->sessionresume,
                 b->voltage, b->position);
        for (auto *node : this->nodes_)
          node->on_clock_offset(static_cast<float>(offset_s));
      }
      else
      {
        // A node with no clock is the I8 case: it will refuse to sleep against a
        // schedule until TimeSync reaches it.  Worth an explicit log line rather
        // than silently publishing nothing.
ESP_LOGI(TAG, "[%s] Beacon: reason=%s reset=%s clock=INVALID fw=%u resume=%d — TimeSync pending",
                 this->get_name().c_str(), reason,
                 s_reset_reason_name(b->resetreason),
                 (unsigned) b->fwversion, (int) b->sessionresume);
      }
    }

    void LORAListener::sync_auto_mode_switch_(const ::NodeWakeBeacon *b)
    {
      // P4b: reflect the node's ACTUAL mode in Home Assistant.
      //
      // Requested mode and real mode legitimately diverge: the node refuses auto
      // mode without a valid clock or a usable schedule, and a physical button
      // press flips it back to interactive on its own. Publishing what the node
      // reports — rather than what was asked for — is what stops the HA toggle
      // from lying about a blind that is actually still interactive.
      //
      // But ONLY once the node is actually on our version. Until then its
      // reported mode is simply out of date — it is telling us what it was
      // doing before our change reached it — and publishing that overwrites the
      // request with the very state the user just changed away from.
      //
      // Observed live: switching auto mode OFF while the node slept, the next
      // beacon arrived 2 s before the push carrying INTERACTIVE went out, and
      // the switch snapped back to ON. The change was still delivered and the
      // node did go interactive, so this was the UI lying rather than the
      // command being lost — but it lies until the NEXT beacon, which on the
      // configured 6 h check-in means six hours of showing the wrong mode.
      //
      // schedule_pending() is exactly "the node is not on our version yet", so
      // it separates the two cases that matter. A button press does NOT set it
      // (the override is local to the node and changes no version), so the
      // button still correctly wins here — which is the case this correction
      // was written for.
      if (this->auto_mode_switch_ != nullptr && !this->schedule_pending())
      {
        const bool node_is_auto = (b->mode == NODE_MODE__MODE_AUTO);
        if (this->auto_mode_switch_->state != node_is_auto)
        {
          ESP_LOGI(TAG, "[%s] Node reports %s — correcting the HA switch",
                   this->get_name().c_str(), node_is_auto ? "AUTO" : "INTERACTIVE");
          this->auto_mode_switch_->publish_state(node_is_auto);
        }
      }
      else if (this->auto_mode_switch_ != nullptr)
      {
        ESP_LOGD(TAG, "[%s] Beacon reports %s but our change is still undelivered "
                      "— leaving the switch as requested",
                 this->get_name().c_str(),
                 (b->mode == NODE_MODE__MODE_AUTO) ? "AUTO" : "INTERACTIVE");
      }
    }

    // Whether a TimeSync is wanted now, and if so the throttle clock is stamped.
    // The request's admit step: the delivery mechanism has already ruled out "no
    // radio" and "session being rebuilt" (kTimeSyncPolicy defers in a relogin, and
    // confirm_session_ sends the TimeSync again 750 ms after the node proves the
    // key), so this is only what is specific to a TimeSync.
    bool LORAListener::timesync_due_()
    {
      if (this->time == nullptr || !this->time->now().is_valid())
      {
        // Home Assistant time not up yet. Skip rather than send epoch 0 — the
        // next login re-runs this, and the node keeps whatever clock it has.
        ESP_LOGW(TAG, "[%s] TimeSync skipped — hub clock not valid yet",
                 this->get_name().c_str());
        return false;
      }

      // Mode B and plain interactive nodes are unthrottled — this only narrows
      // an AUTOMATIC-mode node's resync to once a week (TimeSyncPolicy.h). The
      // first send for a node (last_timesync_sent_us_ == 0) always goes
      // through: shouldRunAutoMode() cannot evaluate the schedule without it.
      const int64_t timesync_now_us = esp_timer_get_time();
      const int64_t elapsed_us = (this->last_timesync_sent_us_ != 0)
                                     ? timesync_now_us - this->last_timesync_sent_us_
                                     : -1;
      if (!timesyncpolicy::shouldSend(this->auto_mode_, elapsed_us))
      {
        ESP_LOGD(TAG, "[%s] TimeSync skipped — automatic-mode weekly resync not due yet",
                 this->get_name().c_str());
        return false;
      }
      this->last_timesync_sent_us_ = timesync_now_us;
      return true;
    }

    // Anything that might mean more traffic for this node (TimeSync's sleepOk is
    // the negation). (Moved out of send_timesync unchanged.)
    bool LORAListener::more_traffic_owed_()
    {
      return this->schedule_pending() ||          // a ScheduleConfig push is due
             this->sched_dirty_ ||                // an edit not yet turned into one
             this->ctl().awaiting_ack ||          // a command awaiting its ack
             !this->session_confirmed_;           // login still settling
    }

    void LORAListener::send_timesync()
    {
      // The payload is owned by a capture shared by the fill and the log, so the
      // pointer the mechanism packs from outlives the call.
      const TimeSync initial = TIME_SYNC__INIT;
      auto ts = std::make_shared<TimeSync>(initial);

      DeliveryRequest req;
      req.what    = "TimeSync";
      req.policy  = deliverypolicy::kTimeSyncPolicy;
      req.admit   = [this]() { return this->timesync_due_(); };
      req.fill    = [this, ts](hubframes::Down *op) { this->fill_timesync_(op, ts.get()); };
      req.on_sent = [this, ts](uint32_t msgid) {
        ESP_LOGI(TAG, "[%s] TimeSync sent (epoch=%llu utcoffset=%+d s msgid=%u)",
                 this->get_name().c_str(), (unsigned long long) ts->epoch,
                 (int) ts->utcoffset, (unsigned) msgid);
      };
      this->deliver_(CH_NONE, std::move(req));
    }

    void LORAListener::fill_timesync_(hubframes::Down *op, TimeSync *ts)
    {
      const auto     now       = this->time->now();
      const uint64_t epoch     = static_cast<uint64_t>(now.timestamp);
      const int32_t  utcoffset = esphome::ESPTime::timezone_offset();

      ts->epoch     = epoch;
      ts->utcoffset = utcoffset;
      ts->dstnext   = 0;   // not yet computed; the node treats 0 as "unknown"

      // U-4: what the hub has OBSERVED of this node's uplink placement.
      //
      // The node's own promotion criterion needs it and cannot measure it: the
      // placement is produced by the node's transmit path and the question is
      // where the frame ARRIVED, which only this end knows. It was hardcoded
      // optimistic on the node, so Demotion::NotConfirmed could never fire.
      //
      // Same counter the hub keeps as a diagnostic (noteUplinkPlacement_), not
      // a second measurement — one definition, and the node's view of it can
      // only ever be this one's echo.
      ts->inslotuplinks = this->belief_.in_slot_acks;

      // TIER 1: tell the node whether it may sleep immediately.
      //
      // The node otherwise waits out a fixed 20 s of hub silence, because
      // silence is its only evidence that nothing more is coming. That wait is
      // 73% of a 28 s check-in wake. The hub does not have to be guessed at --
      // it knows exactly what it still owes this node.
      //
      // Conservative by construction: anything that might mean more traffic
      // leaves sleepOk false, which is precisely today's behaviour. A false
      // negative costs one wake's power; a false POSITIVE drops a command.
      ts->sleepok = !this->more_traffic_owed_();
      op->cmd_case = hubframes::DL_TIMESYNC;
      op->timesync = ts;
    }

    // The CoverConfig, as a Fire request. The cover component builds the payload;
    // the header, msgid, encryption and placement are the mechanism's.
    bool LORAListener::send_cover_config(const ::CoverConfig &cc)
    {
      bool sent = false;
      DeliveryRequest req;
      req.what    = "downlink";
      req.policy  = deliverypolicy::kConfigPushPolicy;
      req.fill    = [payload = cc](hubframes::Down *op) mutable {
        op->cmd_case    = hubframes::DL_COVERCONFIG;
        op->coverconfig = &payload;
      };
      req.on_sent = [&sent](uint32_t) { sent = true; };
      this->deliver_(CH_NONE, std::move(req));
      return sent;
    }

    void LORAListener::send_remote_config()
    {
      ESP_LOGI(TAG, "Sending remote config for device %s", this->address_str());
      ESP_LOGI(TAG, "Setting device name to: %s", this->get_name().c_str());

      // Framed through v1_frame_ (by the mechanism) so it is ENCRYPTED whenever
      // a session exists. This used to pack raw and therefore always went out in
      // the clear — and a provisioned node refuses a plaintext ClientConfig,
      // because ClientConfig sets its address, subnet, name and sleep duration
      // and an unauthenticated frame must not be able to re-address a node.
      // The hub then set config_synced_ = true regardless and never retried, so
      // a config change flashed into the hub was lost silently.
      ClientConfig initial = CLIENT_CONFIG__INIT;
      DeliveryRequest req;
      req.what   = "ClientConfig";
      req.policy = deliverypolicy::kConfigPushPolicy;
      req.fill   = [this, payload = initial](hubframes::Down *op) mutable {
        this->fill_client_config_(op, &payload);
      };
      this->deliver_(CH_NONE, std::move(req));
    }

    void LORAListener::fill_client_config_(hubframes::Down *op, ClientConfig *cc)
    {
      cc->mac_addr = this->address_uint64_;
      cc->addr = this->short_address_;
      cc->subnt = this->subnet_address_;
      cc->name.data = (uint8_t *)this->get_name().c_str();
      cc->name.len = strlen(this->get_name().c_str());
      cc->sleepduration = this->sleep_duration_;
      cc->batteryinterval = this->battery_update_interval_;
      op->cmd_case = hubframes::DL_CLIENTCONFIG;
      op->clientconfig = cc;
    }

    // =====================================================================
    // ModeBSupervisor glue. Policy: ModeBSupervisor.h.
    // =====================================================================

    modebsup::Arbiter &LORAListener::mbs_arbiter_()
    {
      static modebsup::Arbiter arbiter;
      return arbiter;
    }

    uint32_t LORAListener::mbs_now_s_() const
    {
      return (uint32_t) (esp_timer_get_time() / 1000000);
    }

    void LORAListener::start_mode_b_supervisor_()
    {
      this->set_interval("mbs_tick", modebsup::kTickS * 1000, [this]() { this->mbs_tick_(); });
    }

    // Only a node whose Timed Mode switch is ON is ever supervised, and never an
    // auto-mode node (it deep-sleeps between events: Mode B cannot hold there).
    bool LORAListener::mbs_supervised_() const
    {
      return this->timed_mode_enabled_ && this->grid_aligned_ && this->parent_ != nullptr &&
             this->parent_->gridStarted() && !this->auto_mode_ &&
             this->node_mode_ != (uint32_t) NODE_MODE__MODE_AUTO;
    }

    // Somebody else's measurement is on the air for this node.
    bool LORAListener::mbs_other_test_() const
    {
      return (this->mode_test_active_ && !this->mbs_owns_test_) || this->mac_ping_active_ ||
             this->drift_test_active_;
    }

    modebsup::Inputs LORAListener::mbs_inputs_() const
    {
      modebsup::Inputs in;
      in.supervised = this->mbs_supervised_();
      in.session_ok = this->session_confirmed_ && !this->relogin_pending_;
      in.awake      = this->is_node_awake_();
      in.s_since_grid_ack =
          this->mbs_grid_acked_()
              ? (uint32_t) ((esp_timer_get_time() - this->last_gridsync_ack_us_) / 1000000)
              : 0xFFFFFFFFu;
      // The node's own word (PhaseReport.timedRxActive), and only since the last
      // session: a login clears have_phase_report_.
      in.promoted       = this->have_phase_report_ && this->belief_.node_timed_rx;
      in.busy           = this->delivery_busy_();
      in.other_test     = this->mbs_other_test_();
      in.warmup_running = this->mode_test_active_ && this->mbs_owns_test_;
      in.report_ok      = this->mbs_report_fresh_();
      return in;
    }

    void LORAListener::mbs_tick_()
    {
      const modebsup::Event ev = this->mbs_.tick(this->mbs_inputs_(), this->mbs_now_s_(),
                                                 esp_random(), mbs_arbiter_(), this->mbs_id_());
      if (ev != modebsup::Event::None)
        this->mbs_apply_(ev);
    }

    void LORAListener::mbs_apply_(modebsup::Event ev)
    {
      const char *who = this->get_name().c_str();
      switch (ev)
      {
        case modebsup::Event::Start:
          this->mbs_start_warmup_();
          break;
        case modebsup::Event::Yield:
          ESP_LOGI(TAG, "[%s] Mode B warm-up: node busy or another test running — retry in %u s",
                   who, (unsigned) modebsup::kYieldRetryS);
          this->mbs_abort_warmup_(true);
          break;
        case modebsup::Event::DropOut:
          ESP_LOGW(TAG, "[%s] Mode B warm-up abandoned (node asleep, session lost or token lost)", who);
          this->mbs_abort_warmup_(false);
          break;
        case modebsup::Event::Succeeded:
          ESP_LOGW(TAG, "[%s] Mode B RE-ESTABLISHED (node phase report %d, ModeTest report %d)",
                   who, (int) (this->have_phase_report_ && this->belief_.node_timed_rx),
                   (int) this->mbs_report_fresh_());
          break;
        case modebsup::Event::Failed:
          ESP_LOGW(TAG, "[%s] Mode B warm-up FAILED (consecutive failures %u) — retry in %u s",
                   who, (unsigned) this->mbs_.failures(),
                   (unsigned) modebsup::backoffS(this->mbs_.failures()));
          break;
        default:
          break;
      }
    }

    // The warm-up primitive: the SAME ModeTest B the bench's "Mode Test B - timed
    // windows" button runs (single-copy placed marks every 1500 ms, production
    // power profile, MAC echo on), for kWarmupS instead of 300 s. Measured to
    // promote in 13.6 s; the node then holds Mode B on its own.
    void LORAListener::mbs_start_warmup_()
    {
      this->mbs_summary_before_ = this->mode_test_summary_;
      this->mbs_starting_  = true;
      this->start_mode_test(modebsup::kWarmupS, 1500, 2, 1, true, false, false, true);
      this->mbs_starting_  = false;
      ++this->mbs_warmups_started_;
      ESP_LOGW(TAG, "[%s] Mode B warm-up START (attempt %u, %u s of single-copy marks)",
               this->get_name().c_str(), (unsigned) this->mbs_.failures() + 1,
               (unsigned) modebsup::kWarmupS);
    }

    // Give up OUR warm-up without delaying anything: the marks stop at once. With
    // send_stop the node is also told to end its test, but only after the cover
    // op that caused the abort has had its first burst out (and only if nobody
    // started another test meanwhile).
    void LORAListener::mbs_abort_warmup_(bool send_stop)
    {
      if (!this->mbs_owns_test_ || !this->mode_test_active_)
        return;
      this->stop_mode_test_(false);
      if (send_stop)
        this->set_timeout("mbs_stop", 4000, [this]() { this->mbs_send_stop_frame_(); });
    }

    void LORAListener::mbs_send_stop_frame_()
    {
      if (this->mode_test_active_ || this->parent_ == nullptr)
        return;
      this->build_mode_test_frame_(false);
      if (this->mt_frame_len_ > 0)
        this->mac0_send_(this->mt_frame_, this->mt_frame_len_, TxPolicy{}, this->mt_seal_);
    }

    // A cover op / sysop was just issued for this node: never fight it.
    void LORAListener::mbs_yield_to_op_()
    {
      mbs_arbiter_().holdOff(this->mbs_now_s_(), modebsup::kOpHoldOffS);
      this->mbs_yield_self_();
    }

    // Stop THIS node's warm-up for a command about to go out to it, without
    // holding the other nodes' warm-ups off (a schedule push on an auto-mode node
    // is routine and must not postpone a Mode B node's).
    void LORAListener::mbs_yield_self_()
    {
      if (!this->mbs_.yieldToOp(mbs_arbiter_(), this->mbs_id_(), this->mbs_now_s_()))
        return;
      ESP_LOGW(TAG, "[%s] Mode B warm-up aborted: command issued — retry in %u s",
               this->get_name().c_str(), (unsigned) modebsup::kYieldRetryS);
      this->mbs_abort_warmup_(true);
    }

    // The node rebooted, re-logged-in or woke, or the switch changed: the backoff
    // and any warm-up in progress describe a situation that no longer exists.
    void LORAListener::mbs_restart_()
    {
      this->mbs_.restart(mbs_arbiter_(), this->mbs_id_());
      this->mbs_abort_warmup_(false);
    }

    // The node answered OUR warm-up with ArmRefusal::NoGrid: it holds no grid, so
    // the marks are pointless and a blind retry would be refused the same way.
    // Stop the warm-up (quietly) and publish the grid; the failure backoff stays.
    void LORAListener::note_warmup_refusal_()
    {
      const ModeTestSummary &s = this->mode_test_summary_;
      if (!this->mbs_owns_test_ || !this->mode_test_active_ || !s.valid ||
          s.arm_refusal != modebsup::kArmRefusalNoGrid)
        return;
      this->mbs_abort_warmup_(false);
      this->regrid_node_("Node refused the Mode B warm-up: no grid");
    }

    // A GridSync of ours has been acknowledged and nothing newer is outstanding.
    // Derived from the two timestamps rather than kept as a flag, so none of the
    // publish paths has to remember to clear it: a new publish (whose stamp is
    // later than the ack) or a new session (which zeroes the ack stamp) simply
    // makes it false. Strictly later: an ack is always after the publish it answers.
    bool LORAListener::mbs_grid_acked_() const
    {
      return this->last_gridsync_ack_us_ != 0 &&
             this->last_gridsync_ack_us_ > this->last_gridsync_publish_us_;
    }

    // The warm-up's own ModeTest REPORT showed timed windows. A report is "new"
    // when it differs from the one held when the warm-up started - the summary
    // carries no timestamp, and clearing it would blank an operator's last result.
    bool LORAListener::mbs_report_fresh_() const
    {
      const ModeTestSummary &s = this->mode_test_summary_;
      const ModeTestSummary &b = this->mbs_summary_before_;
      const bool changed = s.valid && (!b.valid || s.elapsed_s != b.elapsed_s ||
                                       s.windows_armed != b.windows_armed ||
                                       s.windows_hit != b.windows_hit ||
                                       s.phase_p50_us != b.phase_p50_us);
      return changed && modebsup::reportShowsModeB(s.mode, s.windows_armed, s.windows_hit);
    }

    // The 'Timed Mode' switch restored ON before the radio was up (hub reboot).
    // Sending now would put a GridSync on the air in the clear and without its
    // fleet key (no session yet), then re-publish it five times. So only the
    // intent is recorded; confirm_session_ publishes the grid, encrypted, at the
    // node's first confirmed session, and the supervisor warms the node up after.
    void LORAListener::restore_timed_mode_at_boot_()
    {
      this->timed_mode_enabled_ = true;
      this->set_grid_aligned(true);
      this->belief_               = timedmode::HubBelief{};
      this->last_in_slot_us_      = 0;
      this->have_in_slot_confirm_ = false;
      this->start_mode_b_supervisor_();
      ESP_LOGW(TAG, "[%s] timed mode RESTORED ON at boot — nothing sent yet; the grid is "
                    "published at this node's first confirmed session, then Mode B is warmed up",
               this->get_name().c_str());
    }

    void LORAListener::enable_optimistic_single_shot(bool on)
    {
      this->optimistic_single_shot_ = on;
      ESP_LOGI(TAG, "[%s] optimistic single shot %s", this->get_name().c_str(), on ? "ON" : "OFF");
    }

  } // namespace lora_tracker
} // namespace esphome
