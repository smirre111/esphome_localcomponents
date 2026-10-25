#include <thread>
#include "esphome/components/lora_client/lora_client.h"
// Shared with the node firmware — the AEAD wire format now lives in exactly
// one place. See the banner in FrameCrypto.h.
#include "esphome/components/lora_client/FrameCrypto.h"
// MAC-2 (keys, KDF, Encrypt-then-CMAC, the session key store). See its banner.
#include "esphome/components/lora_client/HubLinkSecurity.h"
// hubframes::Hdr -> mac::MacHdr, shared with the tracker's copy sealer call (S4).
#include "esphome/components/lora_client/MacHdrCodec.h"
// S6: the one oneof -> mac::Port table.
#include "esphome/components/lora_client/PortTable.h"
// S8: MAC wire v1 -- the fixed header (shared) and the hub's bodies around it.
#include "esphome/components/lora_client/MacWire.h"
#include "esphome/components/lora_client/HubWire.h"

// The funnel rate arithmetic, so a ModeTest report is recomputed on the hub
// rather than trusted from the node (test-plan.md I1).
#include "esphome/components/lora_client/MacFunnel.h"
// Section 4.4: the pending-data bitmap the beacon carries.
#include "esphome/components/lora_client/PendingData.h"
// C2: the Class A window offsets the hub aims replies at.
#include "esphome/components/lora_client/ClassAWindows.h"
#include "esphome/components/lora_client/Scheduler.h"
#include "esphome/components/lora_tracker/lora_tracker.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esp_random.h"
#include <algorithm>
#include <map>
#include <memory>
#include <cstring>
#include "psa/crypto.h"
#include <vector>
#include "esphome/components/lora_client/TimedGrid.h"

// ---------------------------------------------------------------------------
// MAC-2 lives in HubLinkSecurity.{h,cpp} since stage S2 (K_root/K_auth, the
// session KDF, Encrypt-then-CMAC, and the SessionKeyStore that is the single
// owner of every peer's session id, keys and generation). The wire is MAC
// wire v1 only (S8, docs/mac-wire-format.md): MacWire.h holds the header,
// HubWire the bodies; the v0 envelope path was deleted in W5.
// ---------------------------------------------------------------------------

static mac2::LinkSecurity &s_sec() { return mac2::LinkSecurity::hub(); }
static mac2::SessionKeyStore &s_keys() { return mac2::LinkSecurity::hub().store(); }

namespace esphome
{
  namespace lora_tracker
  {
    static const char *const TAG = "lora_client";

    namespace
    {
    // Marks an esp_timer callback as running for its whole body, so a stop on
    // the loop can wait for it (LORAListener::wait_callback_idle_, HB2).
    struct CallbackBusy
    {
      explicit CallbackBusy(std::atomic<bool> &b) : busy(b) { busy.store(true); }
      ~CallbackBusy() { busy.store(false); }
      std::atomic<bool> &busy;
    };
    }  // namespace

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

    // Beacon placement (implementation-plan.md 4.4) now lives in TimedGrid.h,
    // with the geometry it belongs to: the tracker TRANSMITS the beacon and
    // this file ANNOUNCES it, and two copies is how those two come to disagree.
    using timedgrid::kBeaconSlotIndex;
    using timedgrid::kBeaconEveryRounds;

    LORAListener::~LORAListener()
    {
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
      // anything else goes out. Our anchor is new; theirs is stale.
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
        this->set_counters_no_write_(0, 0);
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

    void LORAListener::schedule_startup_login_()
    {
      this->cancel_timeout("login_startup");
      this->cancel_timeout("login_retry");

      // Per-node stagger: SLOT × LOGIN_STAGGER_MS, slot being this node's
      // index in declaration order (see login_slot_).
      //
      // Prevents simultaneous login transmissions when all nodes happen to be
      // awake at hub reboot. 3 s gives enough headroom for a full challenge +
      // response exchange even at SF12 (~2.5 s airtime per packet).
      //
      // This used to multiply by short_address_, which gave the right 3 s
      // SEPARATION but an absurd absolute OFFSET: addresses 17 and 18 meant
      // 51 s and 54 s of silence after every hub restart, delaying recovery
      // for both nodes by nearly a minute.
      static constexpr uint32_t LOGIN_STAGGER_MS  = 3000;
      static constexpr uint32_t NODE_BOOT_MARGIN_MS = 5000;
      uint32_t stagger_ms = static_cast<uint32_t>(this->login_slot_) * LOGIN_STAGGER_MS;

      uint32_t delay_ms = this->ms_until_node_awake_();
      if (delay_ms == 0)
      {
        // Node is in its wake window — wait 5 s for radio init + per-node stagger.
        uint32_t total_ms = 5000 + stagger_ms;
        ESP_LOGI(TAG, "[%s] Node awake — login challenge in %u s (5 s radio-init + %u s stagger)",
                 this->get_name().c_str(), total_ms / 1000, stagger_ms / 1000);
        this->set_timeout("login_startup", total_ms, [this]() { this->do_login_and_arm_retry_(); });
      }
      else
      {
        // Add NODE_BOOT_MARGIN_MS beyond the calculated wake time so that REGISTER
        // (sent by the node ~1-3 s after deep-sleep wake-up) almost always arrives
        // before this timer fires and cancels it.  The startup timer is then purely
        // a fallback for lost REGISTER packets.
        uint32_t total_ms = delay_ms + NODE_BOOT_MARGIN_MS + stagger_ms;
        ESP_LOGI(TAG, "[%s] Node asleep — login deferred %u s (%u s sleep + %u s boot margin + %u s stagger)",
                 this->get_name().c_str(), total_ms / 1000,
                 delay_ms / 1000, NODE_BOOT_MARGIN_MS / 1000, stagger_ms / 1000);
        this->set_timeout("login_startup", total_ms,
                          [this]() { this->do_login_and_arm_retry_(); });
      }
    }

    void LORAListener::do_login_and_arm_retry_()
    {
      // Do NOT bail on login_acked_ here.  After a hub reboot the node may have
      // already sent a status message (setting login_acked_=true via the incoming-
      // message path) before this timer fired.  We still need to send LoginMsg so
      // that the base-nonce map is repopulated (it is cleared on hub
      // reboot as a file-scope static and is never persisted to flash).
      //
      // One node's onboarding at a time. The downlink is a single queue at ~1.85 s
      // a frame; a second node's login started behind the first's config, time
      // and schedule pushes sits in that queue while every ack timer runs. Checked
      // before the log so a wait is one line, not one per poll.
      if (this->parent_ != nullptr && !this->acquire_onboarding_())
      {
        this->set_timeout("login_startup", onboarding::kPollMs,
                          [this]() { this->do_login_and_arm_retry_(); });
        return;
      }
      ESP_LOGI(TAG, "[%s] Login challenge firing (parent=%s)",
               this->get_name().c_str(), this->parent_ == nullptr ? "NULL" : "ok");
      if (this->parent_ == nullptr)
      {
        ESP_LOGE(TAG, "[%s] parent_ is null — cannot send login (register_client() missing set_parent?)",
                 this->get_name().c_str());
        return;
      }

      // Reset login state BEFORE send_login(). Today the radio is async so
      // the node's ACK can only arrive long after send_login() returns, and
      // the order doesn't matter. Resetting AFTER would silently clobber an
      // already-arrived ACK the moment the radio path becomes synchronous
      // (e.g. driver swap, scheduler tweak, host-side test) — defensive
      // ordering that matches the production semantic
      // ("login is unacknowledged at the start of a challenge") regardless
      // of when the reply happens to arrive.
      this->login_acked_       = false;
      this->login_retry_count_ = 0;

      this->send_login();   // resets frame counters to 0 + sends LoginMsg

      // Retry every hour; give up after kMaxLoginRetries (24 h).  One-shot,
      // self-re-arming — NOT set_interval (whose random initial phase fired the
      // first "hourly" retry within ~0.6 s, clashing with the register bursts).
      this->cancel_timeout("login_retry");
      this->schedule_login_retry_();
    }

    void LORAListener::schedule_login_retry_()
    {
      // Self-re-arming ONE-SHOT set_timeout with exponential backoff: fast early
      // retries (a transient handshake loss recovers in seconds) growing to the
      // 1 h ceiling.  delay = base << retry_count, capped at kLoginRetryIntervalMs.
      const uint32_t shift = (this->login_retry_count_ < 20) ? this->login_retry_count_ : 20;
      uint64_t delay = static_cast<uint64_t>(kLoginRetryBaseMs) << shift;
      if (delay > kLoginRetryIntervalMs)
        delay = kLoginRetryIntervalMs;
      // The backoff counts from when the login is ON THE AIR, not from when it
      // was queued behind another node's bursts.
      delay = this->ack_wait_ms_(static_cast<uint32_t>(delay));
      // Never retry earlier than the node can hear it. A sleeping node cannot
      // answer, so a burst sent before its wake is pure airtime and hub TX
      // energy — 68 such retries were observed in one log before this gate.
      //
      // The delay is stretched, not replaced: the backoff still governs how
      // often we try once the node IS awake.
      const uint32_t until_awake = this->ms_until_node_awake_();
      if (until_awake > delay)
      {
        ESP_LOGD(TAG, "[%s] Login retry deferred %u s — node asleep until then",
                 this->get_name().c_str(), until_awake / 1000);
        delay = until_awake;
      }

      this->set_timeout("login_retry", static_cast<uint32_t>(delay),
                        [this]() { this->login_retry_step_(); });
    }

    void LORAListener::login_retry_step_()
    {
      if (this->login_acked_)
        return; // acked already; one-shot, nothing to re-arm
      // A retry is a login challenge like any other: it may not start while
      // another node is being brought up. The attempt is NOT counted and the
      // backoff is not advanced — it has not been made yet.
      if (this->parent_ != nullptr && !this->acquire_onboarding_())
      {
        this->set_timeout("login_retry", onboarding::kPollMs,
                          [this]() { this->login_retry_step_(); });
        return;
      }
      if (++this->login_retry_count_ >= kMaxLoginRetries)
      {
        ESP_LOGE(TAG, "[%s] Login not acknowledged after %u retries — giving up",
                 this->get_name().c_str(), static_cast<unsigned>(kMaxLoginRetries));
        this->release_onboarding_("gave up");
        return;
      }
      ESP_LOGW(TAG, "[%s] Login not acknowledged — retry %u/%u",
               this->get_name().c_str(),
               static_cast<unsigned>(this->login_retry_count_),
               static_cast<unsigned>(kMaxLoginRetries));
      this->send_login();
      this->schedule_login_retry_(); // re-arm the next one-shot
    }

    // void LORAListener::send_remote_address()
    // {
    //   ESP_LOGI("LORAListener", "Sending remote address for device %s", this->address_str());
    //   // Implementation to send remote address

    //   BlndOperationMessage op_message BLND_OPERATION_MESSAGE__INIT;
    //   op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
    //   op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
    //   op_message.senderaddress = 0x12345678;
    //   op_message.msgid = 0;                  // Incrementing message ID
    //   op_message.cmd_case = BLND_OPERATION_MESSAGE__CMD_ADDR;

    //   AddressUpdate addr_update = ADDRESS_UPDATE__INIT;
    //   addr_update.addr = this->short_address_;
    //   addr_update.subnt = this->subnet_address_;
    //   op_message.addr = &addr_update;

    //   auto status = false;

    //   uint8_t *txBuf;
    //   unsigned len;
    //   len = blnd_operation_message__get_packed_size(&op_message);
    //   txBuf = new uint8_t[len];
    //   blnd_operation_message__pack(&op_message, txBuf);

    //   // this->parent_->sendPacketOnce(txBuf, len);
    //   this->parent_->send(txBuf, len);

    //   if (status)
    //   {
    //     ESP_LOGW(TAG, "Error writing address command to device");
    //   }

    // }

    // void LORAListener::send_remote_sleep_time()
    // {
    //   ESP_LOGI("LORAListener", "Setting sleep time for device %s", this->address_str());
    //   // Implementation to set sleep time

    //  BlndOperationMessage op_message BLND_OPERATION_MESSAGE__INIT;
    //   op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
    //   op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
    //   op_message.senderaddress = 0x12345678;
    //   op_message.msgid = 0;                  // Incrementing message ID
    //   op_message.cmd_case = BLND_OPERATION_MESSAGE__CMD_SLEEP;

    //   SleepUpdate sleep_update = SLEEP_UPDATE__INIT;
    //   sleep_update.sleepduration = this->sleep_duration_; // Example sleep time
    //   op_message.sleep = &sleep_update;

    //   auto status = false;

    //   uint8_t *txBuf;
    //   unsigned len;
    //   len = blnd_operation_message__get_packed_size(&op_message);
    //   txBuf = new uint8_t[len];
    //   blnd_operation_message__pack(&op_message, txBuf);

    //   // this->parent_->sendPacketOnce(txBuf, len);
    //   this->parent_->send(txBuf, len);

    //   if (status)
    //   {
    //     ESP_LOGW(TAG, "Error writing sleep timecommand to device");

    //   }

    // }

    // A REGISTER is accepted before ANY address or msgid filter, so a node
    // whose address has just changed can still get through.
    //
    // TAKES OWNERSHIP: it frees the message before dispatching the raw bytes
    // onward, because the child nodes re-parse those bytes independently.
    // The frame's K_AUTH tag over every raw byte (spec 4.3).
    bool LORAListener::register_authentic_(const hubframes::Up *rcv_message,
                                           const uint8_t *data, size_t len)
    {
      (void) rcv_message;
      return s_sec().verifyAuthFrameV1(/*downlink=*/false, data, len);
    }

    void LORAListener::handle_register_(hubframes::Up *rcv_message,
                                        uint8_t *data, size_t len)
    {
        ClientRegister *reg = rcv_message->register_;
        ESP_LOGI(TAG, "Received register command for MAC address: %d", reg->mac_addr);
        ESP_LOGI(TAG, "This clients' MAC address: %d", this->address_uint64_);
        if (reg->mac_addr != this->address_uint64_)
        {
          // Every REGISTER reaches every listener; all but the addressed one land
          // here. That is routing, not a fault, and at ERROR it read as one — it
          // was logged twice per handshake and hid the real errors around it.
          ESP_LOGD(TAG, "%s: REGISTER is for another node's MAC — ignoring", this->get_name().c_str());
          return;
        }

        // Tier 3 (mac-separation-implementation-plan.md section 2(b), step
        // 7): verify REGISTER's MIC before any state change — this used to
        // be accepted purely on an address match, before any filter at all,
        // so a forged plaintext REGISTER naming a real node's MAC could drop
        // that node's existing session outright.
        if (!this->register_authentic_(rcv_message, data, len))
        {
          ESP_LOGW(TAG, "%s: REGISTER failed MIC verification — dropping", this->get_name().c_str());
          return;
        }

        // Security review finding 2: the MIC above stops forgery but not
        // replay — REGISTER carries no msgid/freshness check of its own
        // (it is routed around admit_frame_ for the unprovisioned-bootstrap
        // case, which has no session to check one against), and a REGISTER's
        // own msgid is NOT usable as a monotonic replay key either: it is
        // always a small post-reboot value, so a strict increasing check
        // would reject every legitimate reboot after the first. A captured
        // REGISTER could otherwise be replayed at will to force a full
        // relogin cycle on demand — the same grid-demote-with-no-rate-limit
        // bypass H2 closed for LOGIN, reached through this message instead.
        // Rate-limited by time instead: a real reboot cadence is many
        // seconds at minimum, so this blocks rapid replay without blocking
        // a genuine one.
        {
          const int64_t now_us = esp_timer_get_time();
          if (this->last_register_accepted_us_ != 0 &&
              (now_us - this->last_register_accepted_us_) < kRegisterRateLimitUs)
          {
            ESP_LOGW(TAG, "%s: REGISTER rate-limited (%lld ms after the last one) — dropping",
                     this->get_name().c_str(),
                     (long long) ((now_us - this->last_register_accepted_us_) / 1000));
              return;
          }
          this->last_register_accepted_us_ = now_us;
        }

        ESP_LOGI(TAG, "%s, Registered with LORA server", this->get_name().c_str());
        this->registered_ = true;
        // (a'): a REGISTER from THIS node's MAC is proof it is awake right now.
        this->note_node_heard_();

        // §4.6 rule 3: any hub uncertainty means burst. A REGISTER is the hub's
        // actual notification that this node has restarted — it is the first
        // thing a node sends after boot — so it is the one place that can
        // truthfully SET rebooted_since_confirm. The flag defaulted true and
        // was cleared by the first in-slot uplink, but nothing ever raised it
        // again: after the first confirmation it was false for the lifetime of
        // the hub process, and the rule it implements never fired. A rebooted
        // node has lost its grid phase and must re-earn the hub's confidence.
        this->belief_.rebooted_since_confirm = true;
        this->belief_.in_slot_acks           = 0;
        this->have_in_slot_confirm_          = false;
        // The node's last phase report described a grid it held before it
        // restarted. It is not evidence about the node in front of us now.
        this->belief_.phase_reported = false;
        this->belief_.phase_samples  = 0;
        this->have_phase_report_     = false;

        // Only (re)push configuration when the node reports it is unprovisioned
        // (needs_config), or when this hub boot has not yet pushed to the node
        // (!config_synced_ → delivers config changes flashed into the hub once
        // per boot).  A provisioned node waking from deep sleep keeps its
        // persisted config, so both are false and we skip the two ~1.5 s config
        // bursts and go straight to login — saving awake radio time on battery.
        const bool needs_cfg    = reg->needs_config;
        const bool push_config  = needs_cfg || !this->config_synced_;


        // An unprovisioned node holds no session, so plaintext is the only way
        // to reach it and it accepts it — that is the bootstrap. A PROVISIONED
        // node is the opposite case: it has a session and refuses plaintext
        // commands, so pushing now would send two ~1.5 s bursts the node drops
        // on the floor. Defer that push to confirm_session_, which is the first
        // moment the hub can encrypt it.
        if (push_config && needs_cfg)
        {
          ESP_LOGI(TAG, "[%s] Pushing config in the clear (node reports unprovisioned)",
                   this->get_name().c_str());
          this->send_remote_config();   // ClientConfig (address, subnet, name, sleepDuration)
          // Tell every child the node REGISTERed (mac::Port::Session), so each
          // can send its own configuration (LoraCoverComponent: CoverConfig).
          for (auto *node : this->nodes_)
            node->on_register();
          this->config_synced_ = true;
        }
        else if (push_config)
        {
          ESP_LOGI(TAG, "[%s] Config push deferred until the session is confirmed",
                   this->get_name().c_str());
          this->config_push_pending_ = true;
          this->pending_register_notice_ = true;
        }
        else
        {
          ESP_LOGI(TAG, "[%s] Config already in sync — skipping config push, login only",
                   this->get_name().c_str());
        }

        // Re-negotiate nonce and reset frame counters after (re-)registration.
        // Cancel any pending login timers from a previous boot/sleep cycle, then
        // arm a short delay so ClientConfig + node configs are transmitted first.
        this->login_acked_              = false;
        this->login_retry_count_        = 0;
        this->relogin_pending_          = true;  // the node restarted: its old session is gone
        this->pending_login_nonce_      = 0;     // node just (re-)registered → mint fresh
        // Same reasoning as send_login()'s mint-fresh-nonce branch: the node
        // just told us (via REGISTER) that it rebooted, so whatever keys/
        // in-flight tracked-op ciphertext we hold for the OLD session are
        // for a session that, from the node's side, no longer exists either
        // — there is no "retry" to preserve. Without this, clearSessionKeys_()
        // only ran on the hub's NEXT send_login() mint-fresh-nonce call, not
        // here, leaving op_frame_ holding ciphertext the rebooted node could
        // never have decrypted anyway.
        this->clearSessionKeys_();
        this->startup_login_initiated_  = true;  // prevent NTP callback from cancelling this timer
        this->cancel_timeout("login_startup");
        this->cancel_timeout("login_retry");

        // The startup timer (schedule_startup_login_) fires 5 s AFTER the node's
        // expected wake time so that REGISTER (sent by the node ~1-3 s after boot)
        // almost always arrives first and cancels the timer via cancel_timeout above.
        // If REGISTER arrives while the timer is still pending we still schedule the
        // login here — the node's 5-second rate-limit window is wide enough to
        // absorb the rare case where both paths reach do_login_and_arm_retry_() at
        // nearly the same time (the second LoginMsg is simply rate-limited on the node).
        // When config was pushed, kRegisterToLoginDelayMs lets the ClientConfig +
        // node-config bursts drain first so LoginMsg is not queued on top of them.
        // When config was skipped there is nothing to drain, so login fast.
        const uint32_t login_delay_ms = push_config ? kRegisterToLoginDelayMs
                                                     : kRegisterToLoginFastMs;
        ESP_LOGI(TAG, "[%s] REGISTER received — login in %u ms",
                 this->get_name().c_str(), (unsigned)login_delay_ms);
        this->set_timeout("login_startup", login_delay_ms, [this]() {
          this->do_login_and_arm_retry_();
        });
    }

    // Should this frame be acted on? Registration gate, cross-talk address
    // filter, then the bounded replay window — in that order.
    //
    // Does NOT own the message: the caller's RAII holder frees it either way,
    // which is why the manual free at each rejection is gone.
    bool LORAListener::admit_frame_(hubframes::Up *rcv_message)
    {
      if (rcv_message->header) {
        ESP_LOGI(TAG, "Received message with ID %d from address %d", rcv_message->header->msgid, rcv_message->header->senderaddress);
        ESP_LOGI(TAG, "My message ID: %d", this->seq_.rxId());
        ESP_LOGI(TAG, "My address: %d",  this->short_address_);
      } else {
        ESP_LOGI(TAG, "Received message without header");
      }

      if (this->registered_ == false)
      {
        ESP_LOGE(TAG, "%s, Not registered yet, ignoring message", this->get_name().c_str());
        return false;
      }

      if (!(rcv_message->header) || (rcv_message->header->senderaddress != this->short_address_))
      {
        // (b) Cross-talk, already filtered BEFORE decryption: each hub client
        // instance sees every uplink on the shared channel, so a frame from the
        // OTHER node reaching here is normal and expected.  Drop it at debug —
        // this used to be logged at ERROR and fired on every peer frame.
        ESP_LOGD(TAG, "%s, uplink from another node (addr mismatch) — dropping", this->get_name().c_str());
        return false;
      }

      // The message ID is checked only after login
      if (rcv_message->proto_case != hubframes::UL_REGISTER)
      {
        // Accept only a forward jump within a bounded window (mac1::kForwardWindow,
        // 1024) — a huge msgid from a corrupt/spurious frame would otherwise
        // ratchet rx up and wedge the link (dropping every legitimate lower id)
        // until the next login resets it. MAC-1's read-only check (S3).
        if (rcv_message->header && this->seq_.check(rcv_message->header->msgid))
        {
          // CHECKED here, COMMITTED later — see commit_rx_msgid_().
          //
          // This used to assign (and persist) the counter right here, before a
          // single byte had been authenticated. The window is a ratchet, so one
          // forged uplink at msgid = rx + 1024 pushed the hub's expectation past
          // everything the node would legitimately send next, and every real
          // frame was then dropped as "duplicate or old" until the next login.
          // The frame did not have to be acted on to wedge the link; it only had
          // to be admitted. The node has the mirror of this rule in admitFrame().
          //
          // NOTE: login is NOT acknowledged here.  A plaintext status frame (e.g.
          // a position report during boot homing) does not prove the node holds
          // the session key.  Marking login acked on any msgid-bump and then
          // encrypting downlink left a node that missed the LoginMsg unable to
          // decrypt commands.  login_acked_/session_confirmed_ are set only after
          // we successfully DECRYPT a frame from the node (encrypted branch below).
        }
        else
        {
          ESP_LOGE(TAG, "%s, duplicate or old message ID: %d, ignoring. My MsgID: %d",this->get_name().c_str(), (rcv_message->header? rcv_message->header->msgid : 0), this->seq_.rxId());
          return false;
        }
      }

      return true;
    }

    // C2 and §4.6 both hang off ONE measurement: the T0 of this node's own
    // uplink. The only moment the hub can attribute the tracker's receive stamp
    // to a particular node is while the frame that produced it is the one being
    // processed, so it is captured here rather than read later.
    //
    // send_into_class_a_window_() used to read parent_->last_rx_t0_us() directly, 750 ms
    // after the uplink that triggered the reply. That stamp is TRACKER-GLOBAL:
    // one radio, one variable, overwritten by every frame from every node. On a
    // 32-node fleet another node transmitting inside that 750 ms window is the
    // common case, and the reply is a single copy with no burst to save it — so
    // the hub aimed one frame at a window derived from someone else's uplink
    // and the node heard nothing.
    //
    // Called from commit_rx_msgid_, NOT from admit_frame_. Admission checks the
    // address and the replay window, neither of which is authentication: a
    // crafted plaintext frame, or an encrypted one that later fails its CMAC
    // tag, was admitted and moved both of these. That gave an attacker in radio
    // range two levers with no key at all — one out-of-slot frame per round
    // resets in_slot_acks and denies the node single-shot forever, and three
    // frames timed AT the mark earn it for a node the hub has no evidence
    // about. Tying the measurement to the same rule as the counter means a
    // frame moves the hub's beliefs only once it has earned the right to.
    //
    // S5: the stamp is captured with the frame (mac0_ticket_, at admission) and
    // committed here, called by MAC-1 once the frame earned it -- the hub's
    // RxTicket / commitTiming. Same values as before: admission and commit run
    // in one synchronous pass on the loop task, where the tracker writes the
    // stamp before it hands the frame up.
    LORAListener::Mac0Rx LORAListener::mac0_ticket_(size_t len) const
    {
      Mac0Rx rx;
      rx.ticket.len            = static_cast<uint32_t>(len);
      // Spec 4.2: the header has no varints and the tag covers the raw bytes,
      // so the on-air length is always a trustworthy T0 input.
      rx.ticket.timing_trusted = true;
      if (this->parent_ != nullptr)
      {
        rx.ticket.t0_us = this->parent_->last_rx_t0_us();
        rx.unc_us       = this->parent_->rx_stamp_uncertainty_us();
      }
      return rx;
    }

    void LORAListener::mac0_commit_timing_(const Mac0Rx &rx)
    {
      if (this->parent_ == nullptr)
        return;
      this->last_uplink_t0_us_  = rx.ticket.t0_us;
      this->last_uplink_unc_us_ = rx.unc_us;
      // §4.6: the same stamp answers a second question — did this uplink land
      // where the grid says this node transmits?
      this->noteUplinkPlacement_(this->last_uplink_t0_us_);
    }

    // Advance the replay counter, once the frame has earned it.
    //
    // Split out of admit_frame_ so that only two kinds of frame can move it:
    // one the hub has AUTHENTICATED (a successful CMAC tag check), and one that
    // arrived while no session existed, where there is nothing to authenticate
    // with and the counter is the only sequencing there is. A plaintext frame
    // arriving while the session IS confirmed is still processed exactly as
    // before — it just cannot ratchet the counter on its way through.
    void LORAListener::commit_rx_msgid_(const hubframes::Up *m)
    {
      if (m && m->header)
      {
        this->setRxMessageId(m->header->msgid);
        // Keep the plaintext high-water mark with the authenticated counter, so
        // a value left behind by a forged frame cannot outlive the session it
        // was injected into. See the plaintext path in set_response().
        this->plaintext_hwm_      = m->header->msgid;
        this->have_plaintext_hwm_ = true;
        this->last_uplink_msgid_  = m->header->msgid;
      }
      // The same frame, having earned the counter, has earned the measurement:
      // MAC-1 hands MAC-0 the ticket back (commitTiming).
      this->mac0_commit_timing_(this->rx_ticket_);
    }

    // A successful decrypt proves the node holds the matching base nonce, so
    // the encrypted session is confirmed both ways. Everything the hub defers
    // until it can encrypt downlink safely hangs off this one moment: the
    // TimeSync push, the schedule push, and the resume-path config re-login.
    //
    // Deliberately NOT triggered by a plaintext frame bumping the msgid — that
    // does not prove the node holds the session key, and treating it as proof
    // once left a node that missed the LoginMsg unable to decrypt commands.
    // The tracked op that send_cover_operation()/send_tracked_sysop_() deferred
    // behind a relogin, sent now that the session can encrypt it. (Moved out of
    // confirm_session_ unchanged.)
    void LORAListener::send_deferred_tracked_op_()
    {
      this->ctl_hold_for_wake_ = false;   // the node is back
      if (this->ctl().deferred_until_login)
      {
        ESP_LOGI(TAG, "[%s] Session confirmed — sending the deferred tracked op",
                 this->get_name().c_str());
        this->ctl_send_();
      }
    }

    // The config push handle_register_ deferred until the session could encrypt
    // it. (Moved out of confirm_session_ unchanged.)
    void LORAListener::send_deferred_config_push_()
    {
      if (this->config_push_pending_)
      {
        this->config_push_pending_ = false;
        ESP_LOGI(TAG, "[%s] Session confirmed — sending the deferred config push",
                 this->get_name().c_str());
        this->send_remote_config();
        if (this->pending_register_notice_)
        {
          for (auto *node : this->nodes_)
            node->on_register();
          this->pending_register_notice_ = false;
        }
        this->config_synced_ = true;
      }
    }

    void LORAListener::confirm_session_()
    {
      this->session_confirmed_ = true;
      this->relogin_pending_   = false;

      // Security review finding 1: send the tracked op that send_cover_
      // operation()/send_tracked_sysop_() deferred instead of packing
      // plaintext. Unlike the config/TimeSync/GridSync deferrals below, this
      // is NOT gated on !login_acked_ — a tracked op can be deferred by any
      // relogin, not just the hub's first one this process, so it must fire
      // on every confirm that finds one waiting.
      this->send_deferred_tracked_op_();

      // The settle clock for the onboarding gate starts here: the pushes that
      // follow a confirmation are queued over the next ~2 s.
      if (this->onboarding_held_ && !this->onboarding_confirmed_)
      {
        this->onboarding_confirmed_        = true;
        this->onboarding_since_confirm_ms_ = 0;
      }

      // The deferred config push from handle_register_. This is the first
      // moment it can be encrypted, and a provisioned node will not accept it
      // any other way. config_synced_ is set HERE rather than at the point the
      // push was decided, so a push that never happened is not recorded as one
      // that did.
      this->send_deferred_config_push_();
      // U2 (Tier-1 interim mitigation, round 12): clear pending_login_nonce_
      // unconditionally on every confirm, not only on the first ack. Three
      // call paths reach send_login() while login_acked_ is already true
      // (config_sync_relogin, the node-LOGIN path, the missing-nonce path),
      // and each of those previously skipped this clear, so the next
      // send_login() reused the still-pending nonce under a reset msgid —
      // real base-nonce (keystream) reuse. This alone does not fully close the
      // issue (send_login's own unconditional tx/rx reset is the deeper
      // bug; Tier 3's session redesign is the real fix) but removes this
      // specific repeat-clear gap cheaply in the meantime.
      this->pending_login_nonce_ = 0;
      if (!this->login_acked_)
      {
        this->login_acked_         = true;
        this->login_retry_count_   = 0;
        this->acked_since_confirm_ = false;
        this->cancel_timeout("login_retry");
        ESP_LOGI(TAG, "[%s] Login acknowledged by node (encrypted session confirmed)",
                 this->get_name().c_str());

        // P1: seed the node's wall clock.  Deferred rather than sent inline so
        // this frame's processing finishes first and the TimeSync does not
        // pile straight onto the just-completed login exchange — the node is
        // awake and listening for a while yet.  Only now (session_confirmed_)
        // can the node authenticate an encrypted downlink.
        this->set_timeout("timesync_push", 750, [this]() { this->send_timesync(); });

        // D1: give a node its grid back on every confirmed session.
        //
        // MEASURED 2026-09-13. GridSync used to be sent from enable_timed_mode()
        // only — i.e. when someone flipped the switch in Home Assistant. A node
        // that rebooted (power cycle, flash, brownout) lost its grid with its
        // RAM and never got it back: it heard every GridBeacon and dropped each
        // one, because a beacon only re-anchors a grid the node already holds
        // (handleGridBeacon returns on !grid_.active). Three Mode B presses on
        // the bench were refused for "no adopted grid" while Timed Mode showed
        // ON.
        //
        // Once per session, not periodically — deliberately. A rebooted node
        // always logs in again, so this reaches it at the first moment it can
        // authenticate the frame, which is the only moment it can adopt the
        // fleet key the GridSync carries. A periodic GridSync would leave a
        // rebooted node in Mode A for up to a beacon interval and would spend a
        // burst per node per interval on top of the beacon, which is the
        // airtime §4.4's broadcast design exists to save.
        //
        // After the TimeSync and before the schedule push, so this frame's own
        // processing finishes first and the three do not pile onto one round.
        if (this->timed_mode_enabled_ && this->parent_ != nullptr &&
            this->parent_->gridStarted())
        {
          // ONE "grid publish scheduled" timer shared with the beacon-driven
          // re-publish (regrid_node_): whichever trigger comes first, and
          // whatever the order, there is exactly one publish.
          this->regrid_pending_ = true;
          this->set_timeout("gridsync_regrid", 1250, [this]() { this->regrid_fire_(); });
        }

        // F1: push the schedule here TOO, not only from handle_beacon_().
        //
        // The beacon is a single unbursted, unacknowledged frame. When it was
        // the ONLY trigger for the schedule push, losing it blocked automatic
        // mode permanently: no schedule -> node never sleeps -> never wakes ->
        // never beacons again -> hub never re-pushes. Observed live twice, as
        // a hub log with "Login acknowledged" and "TimeSync sent" but no
        // "Beacon:" line and no push.
        //
        // The node is demonstrably listening at this point (it just acked the
        // login), so this is a second, independent path for the initial push.
        // The beacon remains the steady-state trigger — it carries the node's
        // APPLIED version, which is what makes reconciliation self-healing.
        if (this->schedule_pending())
        {
          ESP_LOGI(TAG, "[%s] Schedule pending at login (node=0x%08x hub=0x%08x) — pushing",
                   this->get_name().c_str(), (unsigned) this->node_sched_version_,
                   (unsigned) this->schedule_version());
          this->set_timeout("schedule_push", 2000, [this]() { this->send_schedule_config(); });
        }

        // Resume-path config-sync guarantee: if this session was recovered via
        // the base-nonce RESUME path (NVS-restored counters
        // after a hub reboot) rather than a fresh send_login(), the node never
        // saw a request_register flag — so config changes flashed into the hub
        // this boot would not reach an awake node until its modeled wake.  When
        // config is still unsynced this boot, schedule a fresh login now:
        // send_login() sets request_register = !config_synced_, so a node that
        // supports it responds with a REGISTER and flows through the normal
        // config-push path.  Deferred (not inline) so this frame finishes first.
        // Loop-safe: this whole block runs once per session (guarded by the
        // enclosing !login_acked_), and send_login() does not clear login_acked_,
        // so a node that ignores the flag (older firmware) simply re-acks without
        // re-entering here.  Re-checked in the lambda in case a REGISTER already
        // synced config in the meantime.
        this->schedule_config_sync_relogin_();
      }
    }

    // The resume-path config-sync guarantee of confirm_session_ (moved out
    // unchanged): a fresh login when config is still unsynced this boot.
    void LORAListener::schedule_config_sync_relogin_()
    {
      if (!this->config_synced_)
      {
        ESP_LOGI(TAG, "[%s] Session resumed without config sync this boot — scheduling re-login to push config",
                 this->get_name().c_str());
        this->set_timeout("config_sync_relogin", 500, [this]() {
          // Same reasoning as finding 9's handle_encrypted_ fix: go
          // through do_login_and_arm_retry_(), not a bare send_login(),
          // so this also respects acquire_onboarding_() and arms a retry
          // instead of being a one-shot that silently does nothing if
          // this particular LoginMsg is lost.
          if (!this->config_synced_)
            this->do_login_and_arm_retry_();
        });
      }
    }

    // Tier 3 (mac-separation-implementation-plan.md section 2(b)): derive
    // this listener's K_enc/K_mac from (K_root, session_id, node_nonce,
    // hub/node addresses) — never installed here. Mirrors
    // CmdDispatcher::deriveSessionKeys_() on the node. MAC-2: LinkSecurity.
    bool LORAListener::deriveSessionKeyCandidate_(uint32_t session_id, uint32_t node_nonce,
                                                  mac2::KeyPair *out)
    {
      return s_sec().deriveCandidate(static_cast<mac::Addr>(kHubAddress),
                                     static_cast<mac::Addr>(this->short_address_), session_id,
                                     node_nonce, out);
    }

    // Invariant I3 (c): the store installs under its key lock, racing the
    // tracker task's retag safely. Logged after the lock is released.
    void LORAListener::installSessionKeys_(uint32_t session_id, const mac2::KeyPair &keys)
    {
      const mac::Addr peer = static_cast<mac::Addr>(this->short_address_);
      s_keys().install(peer, session_id, keys);
      this->invalidate_fcnt_reservations_();
      ESP_LOGI(TAG, "[%s] Session keys installed (generation=%u)", this->get_name().c_str(),
               (unsigned) s_keys().generation(peer));
    }

    bool LORAListener::deriveSessionKeys_(uint32_t session_id, uint32_t node_nonce)
    {
      mac2::KeyPair keys;
      if (!this->deriveSessionKeyCandidate_(session_id, node_nonce, &keys))
        return false;
      this->installSessionKeys_(session_id, keys);
      return true;
    }

    void LORAListener::clearSessionKeys_()
    {
      s_keys().clearKeys(static_cast<mac::Addr>(this->short_address_));
      // A reserved FCNT block belongs to the keys just destroyed (P1).
      this->invalidate_fcnt_reservations_();

      // Invariant I3 (a): the ONE place a sealed ciphertext is stored and
      // re-sent later, across however much time passes — pack-once means a
      // retry never re-encrypts. Across a re-login that ciphertext belongs
      // to a session this listener is about to stop holding keys for, so
      // retransmit_tracked_op_() must not be able to resend it: the first
      // frame sealed under the NEW session would otherwise re-approve this
      // STALE one too (both share one dest-keyed generation stamp), and a
      // verified-CMAC-but-wrong-key decrypt would hand the garbage result
      // straight to the response handlers. Dropping it here means the
      // in-flight command is simply lost — the existing kOpMaxRetries/
      // awaiting_ack machinery already handles "no ack arrived" as a
      // user-visible failure, which is what this now also is.
      this->ctl().frame     = MacFrame{};   // the delivery service's stored frame
      this->ctl().frame_tag = 0;
    }

    uint32_t LORAListener::sessionGenerationForTest() const
    {
      return s_keys().generation(static_cast<mac::Addr>(this->short_address_));
    }

    bool LORAListener::ctrEncryptForTest(const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
    {
      return s_sec().ctrEncryptForTest(static_cast<mac::Addr>(this->short_address_), iv, in, len, out);
    }

    bool LORAListener::deriveRootKeyForTest(uint8_t out[16])
    {
      return mac2::LinkSecurity::deriveRootKey(out);
    }

    bool LORAListener::deriveSessionKeysForTest(uint32_t session_id, uint32_t node_nonce)
    {
      s_keys().setSessionId(static_cast<mac::Addr>(this->short_address_), session_id);
      return this->deriveSessionKeys_(session_id, node_nonce);
    }


    uint32_t LORAListener::staleSessionDropsForTest() const
    {
      return s_keys().staleDrops(static_cast<mac::Addr>(this->short_address_));
    }

    // ---- handle_encrypted_ stages ------------------------------------------
    //
    // The crypto of each stage is LinkSecurity's (MacHdr + bytes); what stays
    // here is the order, the logging and the protobuf around it.

    // A candidate not yet installed must be destroyed on every exit that is not
    // the install call at the end of a successful verify+decrypt.
    void LORAListener::uplink_drop_candidate_(EncUplink &up)
    {
      mac2::LinkSecurity::destroy(up.candidate);
    }

    // If we don't have a base nonce for this peer (e.g. recovery after reboot),
    // re-provision one, send it, and discard this packet — the node will restart
    // its encryption with the new nonce on the next transmission.
    //
    // Through send_login(): LoginMsg carries the base nonce and IS the
    // bootstrap (MIC-authenticated, the one plaintext command a node holding
    // a session accepts). BaseNonceExchange, which used to offer a key in
    // plaintext, no longer exists on the wire.
    //
    // This matters most after a HUB reboot: s_base_nonce_map lives in RAM,
    // so the hub comes back holding no nonce for any node while every node
    // still holds its own in NVS. Re-provisioning by a route the node
    // refuses would leave a whole provisioned fleet unreachable until it was
    // re-flashed.
    //
    // PRE-AUTHENTICATION, ACCEPTED (review F7, 2026-10-16,
    // docs/mac-review-2026-10-16.md): this runs BEFORE any CMAC check — there is
    // no key to check one with, which is the point. So ANY frame that merely
    // looks like an encrypted uplink (PROTO_ENCRYPTED, a sender address that
    // passed admit_frame_'s plaintext address filter) from a registered node the
    // hub holds no session for makes the hub start a LOGIN. An attacker in
    // radio range can therefore trigger LOGIN exchanges for such a node, e.g.
    // right after a hub reboot. The cost is one LOGIN exchange (airtime and node
    // battery), bounded by the onboarding gate (acquire_onboarding_) and the
    // login retry ladder. It cannot install keys or confirm a session (the LOGIN
    // is MIC-authenticated and only the real node can answer it); what
    // send_login() does reset — this node's key ids and frame counters on the
    // hub — belongs to a session the hub does not hold anyway. Kept deliberately: refusing would leave a whole fleet unreachable after
    // a hub reboot (see above). Do not "harden" this by checking a tag first.
    //
    // False = the frame is discarded (a login was started).
    bool LORAListener::uplink_check_base_nonce_(const EncUplink &up)
    {
      uint32_t unused_session_id = 0;
      if (up.sender <= 0xFF &&
          s_keys().sessionId(static_cast<mac::Addr>(up.sender), &unused_session_id))
        return true;
      // Security review finding 9: go through do_login_and_arm_retry_(),
      // not a bare send_login(). The bare call bypassed
      // acquire_onboarding_() (the 2026-09-26 onboarding-storm fix —
      // two nodes re-provisioning at once could still pile their
      // ~1.85 s/frame downlink queues onto each other here) and armed
      // no retry, so a dropped re-provisioning LoginMsg had no recovery
      // until this node happened to send another encrypted uplink.
      ESP_LOGW(TAG, "No base nonce for peer %u — re-provisioning via login", up.sender);
      this->do_login_and_arm_retry_();
      return false;
    }

    // Tier 3 (mac-separation-implementation-plan.md section 2(b)): if
    // this IS the session-opening uplink (sessionNonce present), derive
    // a CANDIDATE K_enc/K_mac from it now — into fresh PSA key ids
    // that are NOT installed as this listener's live keys yet.
    //
    // This is what lets the hub follow a node across ANY NUMBER of
    // re-logins within one hub uptime, not just the first: the old code
    // only derived "if this->k_enc_key_id_ == PSA_KEY_ID_NULL", so a
    // node's SECOND-ever login in one hub uptime (a reflash, a REGISTER,
    // a tracked-op giving up and re-logging in, ...) left the hub
    // holding the FIRST session's keys forever — the session-opening
    // uplink's real sessionNonce was simply never looked at again, and
    // every uplink from the node failed CMAC until the hub itself
    // rebooted. installed_session_id_ (set by installSessionKeys_)
    // tracks which session_id the LIVE keys belong to, so a duplicate
    // burst copy of a session-opening uplink ALREADY confirmed (same
    // session_id) is recognised and skipped — it uses the live keys
    // directly, with no new PSA import and no generation bump.
    //
    // Deriving before this frame is verified is safe PROVIDED the
    // candidate is only ever used to try THIS frame, and only INSTALLED
    // (replacing the live keys) if that verify succeeds — a
    // forged sessionNonce then at most wastes one derivation, never
    // pins attacker-chosen keys as live (the earlier version of this
    // comment claimed deriving-and-installing immediately "doesn't
    // move any committed state", which was wrong: it moved exactly the
    // state — this->k_enc_key_id_/k_mac_key_id_ — that every later
    // uplink's decrypt depends on).
    //
    // False = derivation failed, the frame is dropped.
    bool LORAListener::uplink_select_keys_(EncUplink &up)
    {
      // uplink_check_base_nonce_ ran first: the sender has a session id.
      s_keys().sessionId(static_cast<mac::Addr>(up.sender), &up.session_id);
      const mac::Addr self = static_cast<mac::Addr>(this->short_address_);
      up.keys = s_keys().liveKeys(self);
      const hubframes::Hdr *hdr = up.msg->header;
      const bool is_session_opening = hdr && hdr->sessionnonce != 0;
      up.needs_candidate =
          is_session_opening &&
          (!up.keys.hasEnc() || s_keys().installedSessionId(self) != up.session_id);
      if (!up.needs_candidate)
        return true;
      if (!this->deriveSessionKeyCandidate_(up.session_id, hdr->sessionnonce, &up.candidate))
      {
        ESP_LOGE(TAG, "Session key candidate derivation failed for peer %u", up.sender);
        return false;
      }
      up.keys = up.candidate;
      return true;
    }

    // The tag check alone: CMAC over the raw frame (spec 4.1).
    mac2::LinkSecurity::Verify LORAListener::uplink_tag_verdict_(EncUplink &up)
    {
      return this->v1_verify_uplink_(up) ? mac2::LinkSecurity::Verify::Ok
                                         : mac2::LinkSecurity::Verify::BadTag;
    }

    // Authenticate the frame: keys present, CMAC verifies, and the authentic
    // msgid is inside the CTR space. Every failure destroys the candidate.
    bool LORAListener::uplink_verify_(EncUplink &up)
    {
      if (!up.keys.complete())
      {
        ESP_LOGW(TAG, "No session keys derived yet for peer %u — dropping encrypted uplink", up.sender);
        this->uplink_drop_candidate_(up);
        return false;
      }

      const mac2::LinkSecurity::Verify verdict = this->uplink_tag_verdict_(up);
      if (verdict == mac2::LinkSecurity::Verify::NoMem)
      {
        ESP_LOGE(TAG, "Memory allocation failed for MAC input");
        this->uplink_drop_candidate_(up);
        return false;
      }
      if (verdict != mac2::LinkSecurity::Verify::Ok)
      {
        ESP_LOGE(TAG, "Encrypt-then-CMAC verification failed");
        this->uplink_drop_candidate_(up);
        return false;
      }

      // AUTHENTIC, but is its msgid inside the CTR space? The counter block
      // holds 31 bits while the tag covers 32, so msgid X and X + 2^31 would
      // decrypt under the same keystream. Checked only AFTER the tag verified:
      // a forged frame must not be able to make the hub tear a session down.
      // The peer has run its counter off the end; the repair is a fresh
      // session (LOGIN resets both counters), requested once per exhaustion.
      if (!framecrypto::msgidFitsCtr(up.msgid))
      {
        ESP_LOGW(TAG, "[%s] Authentic uplink with msgid %u beyond the CTR space — refusing it",
                 this->get_name().c_str(), (unsigned) up.msgid);
        this->uplink_drop_candidate_(up);
        this->requestMsgidRelogin_("uplink");
        return false;
      }
      return true;
    }

    // CTR-decrypt the verified ciphertext into the listener's own buffer (no
    // heap on the receive path, HB8). Returns it, or nullptr (candidate
    // destroyed) on failure. Loop task only.
    uint8_t *LORAListener::uplink_decrypt_(EncUplink &up)
    {
      const hubframes::Sealed *enc = up.msg->encrypted;
      const size_t cipher_len = enc->ciphertext.len;
      if (cipher_len > sizeof(this->uplink_pt_))
      {
        ESP_LOGE(TAG, "%u B ciphertext does not fit a LoRa frame — dropped", (unsigned) cipher_len);
        this->uplink_drop_candidate_(up);
        return nullptr;
      }
      if (!s_sec().decryptUplink(up.hdr, up.session_id, up.keys, enc->ciphertext.data, cipher_len,
                                 this->uplink_pt_))
      {
        ESP_LOGE(TAG, "CTR decryption failed despite a verified CMAC — this should never happen");
        this->uplink_drop_candidate_(up);
        return nullptr;
      }
      return this->uplink_pt_;
    }

    // The frame is authentic and decrypted: commit it, install the candidate
    // session (if any), prove the node alive, confirm the session, and hand the
    // inner payload on. `plaintext` is uplink_pt_.
    void LORAListener::uplink_accept_(EncUplink &up, uint8_t *plaintext)
    {
      // The tag verified: this frame is the node's, so its msgid is real and
      // the replay counter may finally move.
      this->commit_rx_msgid_(up.msg);

      // The candidate (if one was derived) just proved itself by verifying
      // THIS frame — install it as the live session now, replacing
      // whatever was there before. This is the only path that installs a
      // newly-derived candidate; every early return above destroyed it
      // instead.
      if (up.needs_candidate)
        this->installSessionKeys_(up.session_id, up.candidate);

      // A successful decrypt proves the node holds the matching base nonce —
      // the encrypted session is confirmed both ways.  Only now do we treat
      // login as acknowledged and allow the hub to encrypt downlink commands.
      //
      // (a'): an AUTHENTICATED uplink is also proof the node is awake right
      // now — stamped only past the CMAC check, so a forged or foreign frame
      // cannot move the sleep model.
      this->note_node_heard_();
      this->confirm_session_();

      // The body is the PDU (direction, PORT) names (spec 3).
      this->v1_deliver_decrypted_(up, plaintext, up.msg->encrypted->ciphertext.len);
    }


    // The encrypted uplink path: derive the IV, authenticate, unpack the inner
    // payload, confirm the session, then forward to the child nodes with the
    // plaintext outer header re-attached (loracover needs it for addressing).
    // An ordered pipeline; each stage returns false when the frame is dropped.
    //
    // Does NOT own the message — see admit_frame_.
    void LORAListener::handle_encrypted_(hubframes::Up *rcv_message,
                                         uint8_t *data, size_t len)
    {
      const hubframes::Sealed *enc = rcv_message->encrypted;
      ESP_LOGI(TAG, "Received encrypted payload ciphertext=%d bytes", (int)enc->ciphertext.len);

      // Use the header's msgid as the unified frame counter.  Replay protection
      // was already enforced above (seq_.check(msgid)).
      EncUplink up;
      up.msg    = rcv_message;
      up.sender = rcv_message->header ? rcv_message->header->senderaddress : 0;

      if (!this->uplink_check_base_nonce_(up))
        return;
      if (!this->uplink_select_keys_(up))
        return;
      if (!this->uplink_verify_(up))
        return;
      uint8_t *plaintext = this->uplink_decrypt_(up);
      if (plaintext == nullptr)
        return;
      this->uplink_accept_(up, plaintext);
    }

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

    // A v1 frame is shape-checked and turned into the envelope the rest of
    // set_response walks; anything else (a frame whose VER is not 0x01) is dropped.
    std::unique_ptr<hubwire::Uplink> LORAListener::unpack_uplink_(uint8_t *data, size_t len)
    {
      return this->v1_receive_(data, len);
    }

    void LORAListener::set_response(uint8_t *data, size_t len)
    {
      // The decoded frame, owned for the rest of the function (every exit).
      std::unique_ptr<hubwire::Uplink> owned = this->unpack_uplink_(data, len);
      if (!owned)
      {
        ESP_LOGE(TAG, "Could not read protobuf");
        return;
      }
      hubframes::Up *rcv_message = owned->view();

      this->rx_ticket_ = this->mac0_ticket_(len);   // MAC-0: this frame's timing, held until MAC-1 commits

      // REGISTER first: it must bypass the address and replay filters below, so
      // the order of these two tests is load-bearing.
      if (rcv_message->proto_case == hubframes::UL_REGISTER)
      {
        this->handle_register_(rcv_message, data, len);
        return;
      }

      if (!this->admit_frame_(rcv_message))
        return;

      if (rcv_message->proto_case == hubframes::UL_SEALED &&
          rcv_message->encrypted)
      {
        this->handle_encrypted_(rcv_message, data, len);
        return;
      }

      if (!this->accept_plaintext_uplink_(rcv_message))
        return;
      this->deliver_uplink_(rcv_message, rcv_message->header, /*authentic=*/false);
    }


    // Plaintext path. Before the handshake there is nothing to authenticate
    // with. Such a frame is NOT trusted (review HB4): it never moves the MAC-1
    // receive counter or the timing (a forged FCNT 1000 would make the hub
    // refuse the node's real session-opening uplink, FCNT ~1, as "old", again
    // after every login retry), and it drives no MAC or beacon handling
    // (deliver_uplink_). Duplicates are suppressed by the plaintext high-water
    // mark alone; the application content goes to the children marked
    // unauthentic.
    //
    // ONCE THE SESSION IS CONFIRMED, IT IS REFUSED OUTRIGHT (review finding 1,
    // 2026-09-15). Suppressing duplicates was not enough, because dispatch is
    // not inert and the msgid is plaintext in every header, so anyone in radio
    // range can pick one above the high-water mark and be acted on ONCE, which
    // is all it takes:
    //   * a forged MAC ACK naming a msgid in the tracked command's range
    //     cancels its retransmissions and clears "command failed" — the command
    //     is lost while Home Assistant shows it delivered;
    //   * the same ack confirms a GridSync or a ScheduleConfig push;
    //   * the PhaseReport it carries sets the hub's single-shot belief, which the
    //     plan describes as authenticated by the beacon's CMAC tag — on this path
    //     it was not.
    // This is the mirror of the node's default-deny gate (CmdDispatcher). REGISTER
    // and LOGIN are the two exemptions and both return before this point: REGISTER
    // is the bootstrap, LOGIN carries the base nonce and is the recovery path.
    //
    // Returns true when the frame may be acted on.
    bool LORAListener::accept_plaintext_uplink_(hubframes::Up *rcv_message)
    {
      if (!this->session_confirmed_)
      {
        const uint32_t id = rcv_message->header != nullptr ? rcv_message->header->msgid : 0u;
        if (this->have_plaintext_hwm_ && id <= this->plaintext_hwm_)
        {
          ESP_LOGD(TAG, "[%s] plaintext uplink msgid %u not above %u — duplicate, dropped",
                   this->get_name().c_str(), (unsigned) id, (unsigned) this->plaintext_hwm_);
          return false;
        }
        this->plaintext_hwm_      = id;
        this->have_plaintext_hwm_ = true;
        return true;
      }
      this->plaintext_refused_++;
      ESP_LOGW(TAG, "[%s] refusing a PLAINTEXT uplink (proto_case=%d, msgid=%u) — "
                    "this node holds a confirmed session (%u refused)",
               this->get_name().c_str(), (int) rcv_message->proto_case,
               (unsigned) (rcv_message->header != nullptr ? rcv_message->header->msgid : 0u),
               (unsigned) this->plaintext_refused_);
      return false;
    }

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
      // txround stays 0 while the frame goes out in some later round. That is
      // sound for the ANCHOR, which is the thing the node solves: shifting the
      // declared round by n shifts the solved anchor by n*kRoundUs, and the
      // mark set is invariant modulo kRoundUs, so every mark lands in the same
      // place. It is NOT sound for the round COUNTER, which beacon_every_rounds
      // phases off — hub and node can disagree about which round is a beacon
      // round. Pre-existing, unchanged here, and recorded in the plan.
      // THE ROUND THIS FRAME ACTUALLY GOES OUT IN.
      //
      // It was hardcoded 0, and that was sound for the ANCHOR — shifting the
      // declared round by n shifts the solved anchor by n*kRoundUs, and the
      // mark set is invariant modulo kRoundUs — but not for the round
      // COUNTER, which beacon_every_rounds phases off. With 0 declared, the
      // node numbered its rounds from whenever this frame happened to land,
      // so hub and node disagreed about which round is a beacon round by a
      // random offset, and section 4.4's beacon window would have opened at
      // the wrong time on every node. It is placed on `planned_t0` below, so
      // the round is knowable here rather than approximated.
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
    // BY INTERVAL, not by high-water mark (2026-09-20). This used to call the
    // two-argument nextClearT0ForSlotUs, which floors the search at the END of
    // the latest reservation — so a single-copy mark 1.4 s before a queued
    // beacon was skipped although the air was free. Measured 2026-09-15: two of
    // node 2's marks never sent around a beacon, two empty windows, one short of
    // demotion. The five-argument form was written to replace it and says so in
    // its own comment; only the ModeTest mark scheduler was ever converted.
    //
    // This is the production path — every placed downlink comes through here.
    // A skipped mark costs a whole round of latency on a command someone is
    // waiting for.
    //
    // It used to cost far more: three consecutive empty windows tripped
    // kMaxMissedMarks and demoted the node from one window per round to Mode
    // A's three plus the promotion trial's one, so the hub was causing the
    // exact receive-duty cost Mode B exists to avoid. That demotion was retired
    // on 2026-09-20 — it counted hub silence, not link health — so this fix now
    // buys latency rather than rescuing the node's mode.
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

    // The tracked command's payload (S7): built from the slot's command. The
    // delivery service frames it under a fresh msgid, keeps the frame (pack once,
    // Resend::SameBytes) and re-sends THOSE bytes on every retry.
    //
    // B4: PACK ONCE. Re-packing was two bugs at once. It minted a fresh msgid, so
    // the node's replay filter admitted the retry and executed the command a
    // SECOND time — a blind that moves twice. And re-reading the position at
    // pack time meant a user moving the blind mid-retry produced different
    // plaintext under a msgid-derived CTR counter block; with a reused msgid that
    // is keystream reuse, and with a fresh one it is a duplicate command. Storing
    // the frame removes both: same msgid, same ciphertext, and the node can
    // recognise a duplicate and re-ack it.
    bool LORAListener::encode_ctl_(const DeliveryRequest::FrameFn &frame)
    {
      hubframes::Down op_message {};
      LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
      if (this->ctl().kind == TrackedOpKind::SYSOP)
      {
        op_message.cmd_case = hubframes::DL_SYSOP;
        op_message.sysop    = static_cast<ClientOperation>(this->ctl().sysop);
      }
      else
      {
        op_message.cmd_case = hubframes::DL_OPERATION;
        if (this->ctl().covop_case == LORA_COVER_OPERATION__COVOP_POSITION)
        {
          covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
          covop.position   = this->ctl().position;
        }
        else
        {
          covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
          covop.operation  = static_cast<CovOperation>(this->ctl().operation);
        }
        op_message.operation = &covop;
      }
      // Encrypt the command in-session (CTR + CMAC) so the downlink op is
      // authenticated; falls back to plaintext only before a session exists.
      if (frame(&op_message))
        return true;
      this->ctl().mark_us = 0;
      ESP_LOGE(TAG, "[%s] Failed to pack tracked operation", this->get_name().c_str());
      return false;
    }

    // Where the tracked command goes: aligned, under its supersession generation
    // (a retransmission stays under the generation it belongs to, or a retry
    // would supersede the frame it is a retry of).
    void LORAListener::place_ctl_(const MacFrame &f, const DeliveryAttempt &a)
    {
      // The tracked command downlink is the one B1's gate is about: with
      // alignment on, this is the burst that must observably start on the
      // grid. With it off (the default) behaviour is byte-for-byte as before.
      this->send_aligned_(f.bytes.data(), f.bytes.size(), this->tracked_op_policy_(), f.seal);
      // Rule 4's exposure is THIS command's, so the shape is recorded here
      // rather than inside send_aligned_, where every other producer would
      // overwrite it.
      this->ctl().sent_single_shot = this->last_placed_single_shot_;
      this->ctl().sent_optimistic  = this->last_placed_optimistic_;
      // The placed mark of a FRESH frame (the first, or a re-pack) times the
      // first ack; a re-send of the stored frame leaves it alone.
      if (!a.resent_stored)
        this->ctl().mark_us = this->last_placed_single_shot_ ? this->last_placed_t0_us_ : 0;
    }

    // The Control channel's request: the command the slot describes, delivered by
    // the generic glue. The decisions are the policy's (DeliveryPolicy.h); what is
    // specific to a tracked command is here: the stored frame is re-sent byte for
    // byte, an optimistic single shot gets the short first wait, and a sleep that
    // goes unanswered is not a dead session.
    DeliveryRequest LORAListener::ctl_request_()
    {
      static_assert(deliverypolicy::kControlMaxRetries == kOpMaxRetries &&
                        deliverypolicy::kControlWaitMs == kOpRetryIntervalMs,
                    "the Control policies are the verified ladder");
      DeliveryRequest req;
      req.what        = "Tracked op";
      req.resend_verb = nullptr;   // the service's SameBytes lines log the retransmit, with the frame id
      req.sent_label  = this->ctl().kind == TrackedOpKind::SYSOP ? "Sysop" : "Cover op";
      req.policy      = deliverypolicy::controlPolicy(
          this->ctl().kind == TrackedOpKind::COVER, this->ctl().sysop,
          CLIENT_OPERATION__CMD_SLEEP, CLIENT_OPERATION__CMD_OTA);
      req.encode  = [this](bool, uint32_t *, const DeliveryRequest::FrameFn &frame) {
        return this->encode_ctl_(frame);
      };
      req.place        = [this](const MacFrame &f, const DeliveryAttempt &a) { this->place_ctl_(f, a); };
      req.on_delivered = [this](const DeliveryConfirm &c) { return this->on_ctl_acked_(c.log_id); };
      req.on_fail = [this]() { this->on_ctl_failed_(); };
      // An at-most-once command (OTA) whose stored frame is gone -- a relogin
      // clears it, and the session it was sealed under is dead -- must not be
      // re-packed: that would be a NEW command, and the node may have run the
      // first one already.
      req.resendable  = [this]() {
        return !(this->ctl().req.policy.at_most_once && this->ctl().frame.bytes.empty());
      };
      req.on_abandon  = [this]() { this->on_ctl_abandoned_(); };
      req.on_done     = [this]() { this->ctl_next_(); };
      return req;
    }

    // Start the slot's command: one attempt now, then the ladder.
    void LORAListener::ctl_send_()
    {
      this->ctl().deferred_until_login = false;
      this->ctl().short_wait_spent     = false;
      // A superseded command's ack must not complete this one.
      this->ctl().acks.clear();
      this->set_command_failed_(false);
      this->deliver_(CH_CONTROL, this->ctl_request_());
    }


    bool LORAListener::on_ctl_acked_(uint32_t ack_msg_id)
    {
      this->ctl().awaiting_ack = false;
      this->set_command_failed_(false);
      ESP_LOGI(TAG, "[%s] Tracked op acknowledged (ack_msg_id=%u)", this->get_name().c_str(),
               (unsigned) ack_msg_id);
      if (this->ctl().mark_us != 0)
        this->record_ack_latency_();
      // The nightly sleep takes effect on the hub only now: the node has heard it.
      if (this->op_is_sleep_())
        this->on_sleep_acknowledged_();
      return true;
    }

    // A retry was due but the command cannot be re-sent safely: failed, nothing sent.
    // The session is NOT torn down (the relogin that cleared the frame is the
    // session change; a second one would only repeat it).
    void LORAListener::on_ctl_abandoned_()
    {
      this->ctl().acks.clear();
      this->set_command_failed_(true);
    }

    // Every retry was spent without an ack. (The policy decides whether the
    // session is torn down: not for a sleep, whose silence is ambiguous, see
    // on_sleep_unconfirmed_().)
    void LORAListener::on_ctl_failed_()
    {
      // A late ack of a command given up on must not revive it.
      this->ctl().acks.clear();
      if (this->op_is_sleep_())
      {
        this->on_sleep_unconfirmed_();
        return;
      }
      ESP_LOGE(TAG, "[%s] Tracked op not acknowledged after %u retries — marking failed",
               this->get_name().c_str(), (unsigned) kOpMaxRetries);
      this->set_command_failed_(true);
    }

    bool LORAListener::send_cover_operation(uint32_t covop_case, int32_t operation, float position)
    {
      return this->ctl_submit_({true, covop_case, operation, position, 0});
    }

    void LORAListener::send_tracked_sysop_(int32_t sysop)
    {
      // P1: one-shot sysops used to be sent once, unacked -- a single dropped or
      // replay-rejected frame was lost with no recovery. They go through the same
      // tracked machinery as cover ops: a miss is retransmitted until the node
      // acks, then fails + re-logins.
      this->ctl_submit_({false, 0, 0, 0.0f, sysop});
    }

    bool LORAListener::ctl_busy_() const
    {
      return this->ctl().awaiting_ack || this->ctl().deferred_until_login;
    }

    // One command in flight per node. A newer cover op replaces an older one; any
    // other request waits its turn rather than overwriting what is in flight (a
    // SLEEP, an OTA or a mode switch is never superseded -- and the overwritten
    // SLEEP used to lose its own ack).
    // Returns false when the command was IGNORED because the node sleeps.
    bool LORAListener::ctl_submit_(const deliverypolicy::ControlCmd &c)
    {
      if (this->refuse_while_asleep_(c))
        return false;
      this->mbs_yield_to_op_();
      if (deliverypolicy::admitControl(this->ctl_busy_(), this->ctl().kind == TrackedOpKind::COVER,
                                       c.is_cover) == deliverypolicy::ControlAdmit::Queue)
      {
        this->ctl_enqueue_(c);
        return true;
      }
      this->ctl_begin_(c, false);
      return true;
    }

    void LORAListener::ctl_begin_(const deliverypolicy::ControlCmd &c, bool hold)
    {
      if (c.is_cover)
      {
        this->ctl().kind       = TrackedOpKind::COVER;
        this->ctl().covop_case = c.covop_case;
        this->ctl().operation  = c.operation;
        this->ctl().position   = c.position;
      }
      else
      {
        this->ctl().kind  = TrackedOpKind::SYSOP;
        this->ctl().sysop = c.sysop;
      }
      // A NEW logical command, so a previous one still sitting in the transmit
      // queue is now stale. Bumped before the pack, so the frame carries the
      // generation that supersedes it. See txqueue::SupersedeTable.
      this->ctl().generation++;

      // Security review finding 1: see deferred_until_login's comment. The
      // command's parameters are already stored in the slot, so
      // confirm_session_() can send exactly this once encryption is possible,
      // rather than this sending a plaintext frame now that the node (already
      // holding a session) will reject outright.
      if (this->relogin_pending_ || hold)
      {
        this->ctl().deferred_until_login = true;
        ESP_LOGD(TAG, "[%s] %s deferred — %s", this->get_name().c_str(),
                 c.is_cover ? "Cover op" : "Sysop",
                 hold ? "the node is asleep" : "session being rebuilt");
        return;
      }
      this->ctl_send_();
    }

    void LORAListener::ctl_enqueue_(const deliverypolicy::ControlCmd &c)
    {
      const char *what = c.is_cover ? "Cover op" : "Sysop";
      switch (this->ctl_queue_.push(c))
      {
      case deliverypolicy::ControlQueue::Push::Full:
        // The newest is refused, never an older one: what is queued was asked for.
        ESP_LOGE(TAG, "[%s] %s refused — %u commands are already waiting behind the one in flight",
                 this->get_name().c_str(), what, (unsigned) deliverypolicy::ControlQueue::kDepth);
        this->set_command_failed_(true);
        return;
      case deliverypolicy::ControlQueue::Push::ReplacedCover:
        ESP_LOGI(TAG, "[%s] %s queued behind the command in flight — replaces the cover op already waiting",
                 this->get_name().c_str(), what);
        return;
      case deliverypolicy::ControlQueue::Push::Queued:
        ESP_LOGI(TAG, "[%s] %s queued behind the command in flight (%u waiting)",
                 this->get_name().c_str(), what, (unsigned) this->ctl_queue_.size());
        return;
      }
    }

    void LORAListener::ctl_next_()
    {
      deliverypolicy::ControlCmd c;
      if (!this->ctl_queue_.pop(&c))
        return;
      ESP_LOGI(TAG, "[%s] Starting the %s that waited", this->get_name().c_str(),
               c.is_cover ? "cover op" : "sysop");
      this->ctl_begin_(c, this->ctl_hold_for_wake_);
    }

    TxPolicy LORAListener::tracked_op_policy_() const
    {
      TxPolicy p;
      p.supersede_key = this->short_address_;
      p.supersede_gen = this->ctl().generation;
      return p;
    }


    // ---- the generic delivery glue (step 3 of the unified MAC delivery refactor) ----
    //
    // One request per channel. The DECISIONS (when to retry, when to give up, which
    // ack counts) are deliverypolicy::; this only performs them. The Control channel
    // carries cover operations and sysops, the Config channel the ScheduleConfig
    // push, the Mac channel the GridSync publish.

    const char *LORAListener::dlv_timer_(DeliveryChannel ch)
    {
      static const char *const kNames[kDeliveryChannels] = {"dlv_ctl", "dlv_cfg", "dlv_mac"};
      return kNames[ch];
    }

    void LORAListener::deliver_(DeliveryChannel ch, DeliveryRequest req)
    {
      // A Fire request is sent once and occupies no channel: it never reaches the
      // slot below, so it cannot cancel, replace or delay a request in flight on any
      // channel (a TimeSync goes out between a ScheduleConfig's attempts).
      if (req.policy.mode == deliverypolicy::Mode::Fire)
      {
        this->dlv_fire_(req);
        return;
      }
      if (ch >= kDeliveryChannels)
        return;   // an acked request needs a channel to wait on
      DeliverySlot &s = this->slots_[ch];
      // A request already in flight is REPLACED by this one, with a fresh ladder
      // of its own (the budget is per request). Its msgids stay on record in
      // s.acks, so a late ack for the old request is still recognised. Its timer
      // needs no cancelling: the first attempt re-arms the same named one, and a
      // request that never arms one leaves it to find nothing awaited.
      s.req          = std::move(req);
      s.retry_count  = 0;
      s.awaiting_ack = false;
      // The stored frame belongs to the request being replaced. A new request
      // whose first pack fails must re-pack (or, at-most-once, be abandoned) on
      // its retry, never re-send the previous command's bytes (review HB3).
      s.frame        = MacFrame{};
      s.frame_tag    = 0;
      if (s.req.policy.yields_warmup)
        this->mbs_yield_self_();
      this->dlv_tx_(ch, false);
    }

    // THE one transmission of a Fire request (step 6): TimeSync, ClientConfig,
    // CoverConfig. Everything that used to be repeated at each of those three call
    // sites lives here once -- the relogin deferral, the msgid, the header, the
    // encryption decision (v1_frame_: sealed whenever a session is confirmed,
    // plaintext only before one exists), the shape and the failure log. What the
    // frame SAYS, and when it is wanted at all, is the request's.
    bool LORAListener::dlv_fire_(DeliveryRequest &req)
    {
      if (this->parent_ == nullptr || !req.fill)
        return false;   // no radio, or nothing to say
      // See relogin_pending_. Checked before the msgid is taken, so a deferred
      // frame does not burn one; confirm_session_ sends it again.
      if (deliverypolicy::fireDeferred(req.policy, this->relogin_pending_))
      {
        ESP_LOGD(TAG, "[%s] %s deferred — session being rebuilt", this->get_name().c_str(),
                 req.what);
        return false;
      }
      if (req.admit && !req.admit())
        return false;

      hubframes::Down op_message {};
      req.fill(&op_message);

      MacFrame f;
      if (!this->mac_frame_(&op_message, &f))
      {
        ESP_LOGE(TAG, "[%s] Failed to pack %s", this->get_name().c_str(), req.what);
        return false;
      }
      const bool sent = this->fire_place_(req.policy.shape, f, req.what);
      if (sent && req.on_sent)
        req.on_sent(f.msgid);
      return sent;
    }

    bool LORAListener::fire_place_(deliverypolicy::Shape shape, const MacFrame &f, const char *what)
    {
      const uint8_t *buf = f.bytes.data();
      const size_t   len = f.bytes.size();
      switch (shape)
      {
      case deliverypolicy::Shape::Bare:
        return this->mac0_send_(buf, len, TxPolicy{}, f.seal);
      case deliverypolicy::Shape::Auto:
        return this->send_aligned_(buf, len, f.seal);
      case deliverypolicy::Shape::Burst:
      {
        TxPolicy p;
        p.copies = this->parent_->defaultBurstCopies();
        return this->send_aligned_(buf, len, p, f.seal);
      }
      case deliverypolicy::Shape::ClassA:
        return this->fire_class_a_(f, what);
      }
      return false;
    }

    // C2 when the hub has a usable uplink stamp for this node, today's burst
    // otherwise. Not a switch: the fallback IS the old behaviour, so a node the hub
    // cannot place still gets its frame the way it always did.
    bool LORAListener::fire_class_a_(const MacFrame &f, const char *what)
    {
      const ClassAPlacement placement =
          this->send_into_class_a_window_(f.bytes.data(), f.bytes.size(), f.seal);
      if (placement == ClassAPlacement::Placed)
        return true;

      if (placement == ClassAPlacement::WindowsPast)
      {
        // The node is ASLEEP, and this end is the one that knows it.
        //
        // The burst below rests on "a TimeSync carries no ack, so a lost
        // single copy is silent" — an argument that assumes something is
        // LISTENING. Both Class A windows have closed: no number of copies
        // is heard, so 17 of them is 1408 ms of air, colliding with every
        // other node's window, to achieve exactly nothing.
        //
        // Dropping costs nothing the burst would have won. The node beacons
        // on its next wake and P2b answers every beacon with a TimeSync
        // 750 ms later — while RX1 is still 250 ms out, so THAT one places.
        ESP_LOGI(TAG, "[%s] %s dropped — both Class A windows past, "
                      "the node is asleep; its next beacon gets one",
                 this->get_name().c_str(), what);
        return false;
      }
      if (placement == ClassAPlacement::NotClassA ||
          placement == ClassAPlacement::NoUplinkStamp)
      {
        // PLACED, and as a BURST. Placement is the fix: through the bare
        // send() this left whenever the queue drained, so in Mode B it
        // arrived outside the node's window and the clock silently did not
        // advance.
        //
        // The burst is asked for EXPLICITLY because a TimeSync carries no
        // ack. Rule 4's exposure argument — "being wrong costs one frame" —
        // rests on the loss being visible; for an unacked frame a missed
        // single copy is silent, and the node runs on a stale clock until
        // the next push. Airtime is the cheaper side of that trade.
        //
        // Both reasons are named rather than negated: these two are the
        // cases where the hub does NOT know where the node listens, and the
        // burst spans every window it might open. WindowsPast is the case
        // where it knows precisely, and knows the answer is "nowhere".
        TxPolicy p;
        p.copies = this->parent_->defaultBurstCopies();
        this->send_aligned_(f.bytes.data(), f.bytes.size(), p, f.seal);
        return true;
      }
      return false;   // NotSendable: nothing to send
    }

    // ONE attempt of the slot's request (S7). A Resend::SameBytes retry re-sends
    // the stored first frame byte for byte under its ORIGINAL msgid -- that is the
    // point, a fresh msgid would be a new command to the node. Every other attempt
    // asks the application for the payload, frames it under a fresh msgid and
    // places it. A SameBytes request whose stored frame is gone (a session change
    // cleared it) is re-packed: a NEW command, so its id range moves with it
    // (security review finding 1: the old range would reject the node's ack).
    // Returns the msgid, 0 when nothing was framed.
    uint32_t LORAListener::dlv_attempt_(DeliverySlot &s, bool retry, uint32_t *tag)
    {
      const bool same_bytes = s.req.policy.resend == deliverypolicy::Resend::SameBytes;
      if (retry && same_bytes && !s.frame.bytes.empty())
      {
        *tag = s.frame_tag;
        if (s.req.place)
          s.req.place(s.frame, DeliveryAttempt{true, true, s.frame_tag});
        return s.frame.msgid;
      }
      if (retry && same_bytes)
        ESP_LOGW(TAG, "[%s] no stored frame to retransmit — re-packing", this->get_name().c_str());
      MacFrame f;
      bool framed = false;
      const DeliveryRequest::FrameFn frame = [this, &f, &framed](hubframes::Down *op) {
        framed = this->mac_frame_(op, &f);
        return framed;
      };
      if (s.req.encode)
        s.req.encode(retry, tag, frame);
      if (framed && s.req.place)
        s.req.place(f, DeliveryAttempt{retry, false, *tag});
      if (same_bytes)
      {
        // Only a frame that was built is ever stored; a failed pack leaves the
        // slot empty (deliver_ cleared it), so the retry re-packs or abandons.
        if (framed)
        {
          s.frame     = f;
          s.frame_tag = *tag;
        }
        if (retry)
          s.first_msgid = f.msgid;   // a re-pack is a new command: its range moves
        return f.msgid;              // a SameBytes id is taken even when packing failed
      }
      return framed ? f.msgid : 0;
    }

    // The attempt lines of a Resend::SameBytes request (the Control channel).
    void LORAListener::dlv_log_attempt_(const DeliverySlot &s, bool retry, uint32_t msgid)
    {
      if (s.req.policy.resend != deliverypolicy::Resend::SameBytes)
        return;
      if (!retry)
      {
        if (s.req.sent_label != nullptr)
          ESP_LOGI(TAG, "[%s] %s sent (msgid=%u) — awaiting ack", this->get_name().c_str(),
                   s.req.sent_label, (unsigned) msgid);
      }
      else if (s.short_wait_spent && s.retry_count == 0)
        ESP_LOGW(TAG, "[%s] %s burst fallback (msgid=%u)", this->get_name().c_str(), s.req.what,
                 (unsigned) msgid);
      else
        ESP_LOGW(TAG, "[%s] %s retransmit %u/%u (msgid=%u)", this->get_name().c_str(), s.req.what,
                 (unsigned) s.retry_count, (unsigned) s.req.policy.max_retries, (unsigned) msgid);
    }

    bool LORAListener::dlv_tx_(DeliveryChannel ch, bool retry)
    {
      DeliverySlot &s = this->slots_[ch];
      uint32_t tag = 0;
      const uint32_t msgid = this->dlv_attempt_(s, retry, &tag);
      this->dlv_log_attempt_(s, retry, msgid);
      if (msgid != 0)
      {
        s.acks.add(msgid, tag);
        s.last_msgid = msgid;
        if (!retry)
          s.first_msgid = msgid;
      }
      // A retry that could not be built still keeps the ladder running to its
      // end; a first attempt that could not be built starts nothing.
      if (s.req.policy.mode == deliverypolicy::Mode::Acked && (msgid != 0 || retry))
      {
        s.awaiting_ack = true;
        this->arm_dlv_timer_(ch);
      }
      return msgid != 0;
    }

    void LORAListener::arm_dlv_timer_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      // The FIRST wait of an optimistic single shot is the short, adaptive one
      // (SingleShotAckWait.h), measured from the placed mark; every other wait is
      // the policy's, plus the transmit backlog.
      const bool short_wait = deliverypolicy::isShortFirstWait(
          s.sent_optimistic, s.retry_count, s.short_wait_spent);
      s.first_tail_ms = 0;
      const uint32_t wait_ms = short_wait ? this->dlv_short_wait_ms_(ch)
                                          : this->ack_wait_ms_(s.req.policy.base_wait_ms);
      this->set_timeout(dlv_timer_(ch), wait_ms,
                        [this, ch, short_wait]() { this->on_dlv_timeout_(ch, short_wait); });
    }

    // The short first ack wait of an optimistic single shot, in ms from NOW.
    // Records the node's mark->ack tail in first_tail_ms.
    uint32_t LORAListener::dlv_short_wait_ms_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      // From NOW: whichever is later of the placed mark and the queue's drain
      // (they estimate the same instant; ack_wait_ms_ must NOT be added on top,
      // it would count the time to the mark twice), plus THIS node's measured
      // mark->ack tail.
      const int64_t  us_to_t0 = ((s.mark_us != 0) ? s.mark_us : this->last_placed_t0_us_) -
                                esp_timer_get_time();
      int64_t drain_ms = (this->parent_ != nullptr) ? this->parent_->txDrainUs() / 1000 : 0;
      if (drain_ms < 0)
        drain_ms = 0;
      if (drain_ms > (int64_t) kMaxAckBacklogMs)
        drain_ms = kMaxAckBacklogMs;
      const uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
      const uint32_t tail   = this->ack_latency_.tailMs(now_ms);
      s.first_tail_ms = tail;
      const uint32_t wait_ms = singleshotwait::firstAckWaitMs(us_to_t0, (uint32_t) drain_ms, tail);
      ESP_LOGI(TAG, "[%s] optimistic single shot: ack deadline %u ms from now "
                    "(mark in %d ms, queue drain %d ms, tail %u ms; node history %u "
                    "samples, max %u ms)",
               this->get_name().c_str(), (unsigned) wait_ms,
               (int) (us_to_t0 / 1000), (int) drain_ms, (unsigned) tail,
               (unsigned) this->ack_latency_.count(now_ms),
               (unsigned) this->ack_latency_.maxDelayMs(now_ms));
      return wait_ms;
    }

    void LORAListener::on_dlv_timeout_(DeliveryChannel ch, bool short_wait)
    {
      DeliverySlot &s = this->slots_[ch];
      const deliverypolicy::TimeoutDecision d = deliverypolicy::onAckTimeout(
          {s.awaiting_ack, this->relogin_pending_, short_wait, s.retry_count, s.req.policy.max_retries});
      s.retry_count = d.retries_spent;
      switch (d.action)
      {
      case deliverypolicy::TimeoutAction::Stale:
        return;   // acked already; one-shot, nothing to re-arm
      case deliverypolicy::TimeoutAction::DeferRelogin:
        // No session to answer on (a relogin can start WHILE a retry is in
        // flight, and clears the stored frame): a frame sent now would be a
        // plaintext one the node, still holding its old session, rejects. Wait for
        // confirm_session_(), charge nothing.
        ESP_LOGD(TAG, "[%s] %s retry deferred — session being rebuilt", this->get_name().c_str(),
                 s.req.what);
        this->arm_dlv_timer_(ch);
        return;
      case deliverypolicy::TimeoutAction::ShortWaitMissed:
        this->dlv_short_wait_missed_(ch);
        return;
      case deliverypolicy::TimeoutAction::GiveUp:
        this->dlv_give_up_(ch);
        return;
      case deliverypolicy::TimeoutAction::Retransmit:
        break;
      }
      this->dlv_resend_(ch);
    }

    void LORAListener::dlv_resend_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      if (s.req.resendable && !s.req.resendable())
      {
        this->dlv_abandon_(ch);
        return;
      }
      // Rule 4: a single shot that went unacked puts this node back on bursts
      // immediately, and stays that way until a fresh in-slot uplink. One frame is
      // the whole exposure -- the retry below is already a burst.
      if (deliverypolicy::marksSingleShotUnacked(s.sent_single_shot, this->belief_.single_shot_unacked))
      {
        this->belief_.single_shot_unacked = true;
        ESP_LOGW(TAG, "[%s] single shot unacked — back to bursts until "
                      "the node is confirmed again", this->get_name().c_str());
      }
      if (s.req.resend_verb != nullptr)
        ESP_LOGW(TAG, "[%s] %s not acknowledged — %s (%u/%u)", this->get_name().c_str(),
                 s.req.what, s.req.resend_verb, (unsigned) s.retry_count,
                 (unsigned) s.req.policy.max_retries);
      this->dlv_tx_(ch, true);
    }

    void LORAListener::dlv_done_(DeliveryChannel ch)
    {
      const std::function<void()> done = this->slots_[ch].req.on_done;
      if (done)
        done();
    }

    void LORAListener::dlv_abandon_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      s.awaiting_ack = false;
      ESP_LOGE(TAG, "[%s] %s may already have been heard and can no longer be re-sent as the "
                    "same command (its session changed) — failed, NOT re-sent",
               this->get_name().c_str(), s.req.what);
      if (s.req.on_abandon)
        s.req.on_abandon();
      else if (s.req.on_fail)
        s.req.on_fail();
      this->dlv_done_(ch);
    }

    // The optimistic guess missed. Burst NOW (Rule 4 marks the node unacked, so
    // the resend places a full burst), but do NOT count it: retry_count is the
    // budget that ends in "command failed, tear the session down", and a guess the
    // operator opted into is not evidence the session is dead. The normal waits and
    // the full budget follow unchanged.
    void LORAListener::dlv_short_wait_missed_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      s.short_wait_spent = true;
      this->belief_.single_shot_unacked = true;
      // The tail just proven too short is a lower bound on this node's delay:
      // remember it, so the next deadline is longer instead of the same mistake
      // (and a node that never answers climbs to the normal wait, no further).
      this->ack_latency_.note((uint32_t) (esp_timer_get_time() / 1000), s.first_tail_ms);
      ESP_LOGW(TAG, "[%s] optimistic single shot unacked — bursting now "
                    "(not counted against the retry budget)",
               this->get_name().c_str());
      this->dlv_tx_(ch, true);
    }

    void LORAListener::dlv_give_up_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      s.awaiting_ack = false;
      if (s.req.on_fail)
        s.req.on_fail();
      if (s.req.policy.give_up == deliverypolicy::GiveUpAction::FailRelogin)
      {
        // Session recovery. A command that goes fully unacked means the session is
        // effectively dead -- the hub never confirmed it, OR it had, but the node
        // has since re-keyed/hung leaving a STALE-confirmed flag (observed
        // 2026-07-11: node 1 failed 4x without recovering because session_confirmed_
        // was still true). Clear the confirmation and force an immediate fresh login
        // so the next command rebuilds the session in seconds.
        this->session_confirmed_ = false;
        ESP_LOGW(TAG, "[%s] Command failed after retries — clearing session, forcing re-login",
                 this->get_name().c_str());
        this->do_login_and_arm_retry_();
      }
      this->dlv_done_(ch);
    }

    // The node acknowledged `ack_msg_id`. A request that owns it is told which
    // attempt was answered (its tag) and says whether that completes it.
    bool LORAListener::dlv_ack_(uint32_t ack_msg_id)
    {
      // Highest channel first: a GridSync takes an id before a schedule push would.
      for (uint8_t c = kDeliveryChannels; c-- > 0;)
      {
        DeliverySlot &s = this->slots_[c];
        const int i = s.acks.find(ack_msg_id);
        if (i < 0)
          continue;
        if (!s.req.on_delivered || s.req.on_delivered(DeliveryConfirm{s.acks.tag[i], ack_msg_id}))
        {
          this->dlv_cancel_((DeliveryChannel) c);
          this->dlv_done_((DeliveryChannel) c);
        }
        return true;
      }
      return false;
    }

    void LORAListener::dlv_cancel_(DeliveryChannel ch)
    {
      DeliverySlot &s = this->slots_[ch];
      this->cancel_timeout(dlv_timer_(ch));
      s.awaiting_ack = false;
      s.retry_count  = 0;
      s.acks.clear();
    }

    // A new login restarts the msgid space, so ids on record would alias new ones.
    // The requests themselves go on (the next attempt is sent under a new id).
    void LORAListener::dlv_forget_ids_()
    {
      for (uint8_t c = CH_CONFIG; c < kDeliveryChannels; ++c)
        this->slots_[c].acks.clear();
    }

    bool LORAListener::delivery_busy_() const
    {
      return this->ctl().awaiting_ack || this->ctl().deferred_until_login ||
             this->cfg().awaiting_ack || this->mac().awaiting_ack;
    }

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

    // Mark -> ack, for every command whose FIRST frame was a single placed
    // copy (one INFO per user command: this is not a periodic log). It feeds
    // the node's latency history only when clean (no retry, no fallback burst).
    void LORAListener::record_ack_latency_()
    {
      const int64_t now_us   = esp_timer_get_time();
      const int64_t delay_ms = (now_us - this->ctl().mark_us) / 1000;
      const bool clean = singleshotwait::isCleanSample(
          true, this->ctl().retry_count, this->ctl().short_wait_spent, true);
      bool kept = false;
      if (clean && delay_ms >= 0)
        kept = this->ack_latency_.note((uint32_t) (now_us / 1000), (uint32_t) delay_ms);
      ESP_LOGI(TAG, "[%s] ack %lld ms after the mark (%s%s; first-ack tail %u ms; "
                    "history now %u samples, max %u ms)",
               this->get_name().c_str(), (long long) delay_ms,
               clean ? "clean single shot" : "after a burst fallback or retry",
               kept ? ", recorded" : ", not recorded", (unsigned) this->ctl().first_tail_ms,
               (unsigned) this->ack_latency_.count((uint32_t) (now_us / 1000)),
               (unsigned) this->ack_latency_.maxDelayMs((uint32_t) (now_us / 1000)));
    }

    void LORAListener::handle_command_ack_(uint32_t ack_msg_id, AckStatus status)
    {
      this->acked_since_confirm_ = true;
      if (status == ACK_STATUS__ACK_OK)
        this->dlv_ack_(ack_msg_id);
      else
        this->dlv_refused_(ack_msg_id, status);
    }

    // The node HEARD an Acked frame and answered that it will not execute it:
    // ACK_ERROR for a body it cannot dispatch (a case newer than its firmware,
    // review P3), ACK_BUSY reserved. Retrying the same bytes cannot change that,
    // and the session is demonstrably alive, so the request fails at once:
    // no retries, no relogin (unlike a ladder that ran out without any answer).
    bool LORAListener::dlv_refused_(uint32_t ack_msg_id, AckStatus status)
    {
      for (uint8_t c = kDeliveryChannels; c-- > 0;)
      {
        DeliverySlot &s = this->slots_[c];
        if (s.acks.find(ack_msg_id) < 0)
          continue;
        ESP_LOGE(TAG, "[%s] %s refused by the node (status %d, FCNT %u) — failed, not retried",
                 this->get_name().c_str(), s.req.what, (int) status, (unsigned) ack_msg_id);
        this->dlv_cancel_((DeliveryChannel) c);
        if (s.req.on_abandon)
          s.req.on_abandon();
        else if (s.req.on_fail)
          s.req.on_fail();
        this->dlv_done_((DeliveryChannel) c);
        return true;
      }
      return false;
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
        ESP_LOGI("LORAListener",
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

      ESP_LOGI("LORAListener", "Device %s entering sleep mode (tracked, effects on ack)",
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
      ESP_LOGI("LORAListener", "Device %s triggering OTA", this->address_str());
      // P1: OTA is a one-shot sysop — send it through the tracked/acked path so a
      // dropped or replay-rejected frame is retransmitted until the node acks,
      // instead of being silently lost (the observed post-reboot OTA drop).  The
      // sysop is encrypted in-session by tx_tracked_op_() so it cannot be forged.
      this->send_tracked_sysop_(CLIENT_OPERATION__CMD_OTA);
    }

    void LORAListener::sendDebugSysop(int32_t op)
    {
      ESP_LOGI("LORAListener", "Device %s: debug sysop %d", this->address_str(), (int) op);
      // Same tracked/acked path as every other sysop -- a dropped toggle
      // recovers via the hub's own retransmit rather than silently not
      // taking effect. See DebugFlags.h (node repo) for what each op does.
      this->send_tracked_sysop_(op);
    }


    uint32_t LORAListener::incrTxMessageId()
    {
      // LOOP ONLY: allocating persists the counter through ESPHome
      // preferences (persist_hook_ -> save_state_), which the loop task owns.
      // The esp_timer streams (MAC ping, ModeTest marks, drift ticks) take
      // their FCNTs from a block reserved on the loop (reserve_fcnt_block_)
      // and never reach this. The lock guards the counter against
      // take_reserved_fcnt_ running concurrently.
      uint32_t val = 0;
      bool exhausted = false;
      {
        std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
        const mac1::Alloc a = this->seq_.allocate(this->persist_());
        if (a.status != mac1::AllocStatus::Ok)
        {
          // The CTR counter block holds 31 bits of msgid: handing out 2^31 would
          // reuse the keystream of msgid 0. Do not advance the counter (it must
          // not wrap); hand back the first out-of-space id, which the seal point
          // refuses, and ask for a fresh session below.
          exhausted = true;
          val = framecrypto::kMsgidCtrLimit;
        }
        else
        {
          // Persisted before it is handed out (persist_hook_, margin 1), so a
          // reboot never reuses an id.
          val = a.fcnt;
        }
      }
      if (exhausted)
        this->requestMsgidRelogin_("allocate");
      return val;
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

    void LORAListener::requestMsgidRelogin_(const char *where)
    {
      // Once per exhaustion: the flag is cleared in send_login(), which is where
      // both msgid counters restart. Until then every refusal is silent.
      if (this->msgid_relogin_pending_)
        return;
      this->msgid_relogin_pending_ = true;
      ++this->msgid_relogin_requests_;
      ESP_LOGE(TAG, "[%s] msgid space exhausted (%s): the CTR keystream would repeat — "
                    "starting a fresh session", this->get_name().c_str(), where);
      // Deferred, not inline: callers sit inside frame building and the
      // admission path, and a login from there would re-enter them.
      this->set_timeout("msgid_relogin", 0, [this]() { this->do_login_and_arm_retry_(); });
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

    bool LORAListener::mac0_send_(const uint8_t *buf, size_t len, TxPolicy policy,
                                  const mac2::FrameSeal &seal)
    {
      // A copy is sealed over its raw bytes (spec section 5), never by decoding
      // a header.
      policy.sealer       = seal.sealed ? &mac2::LinkSecurity::copySealerV1 : nullptr;
      policy.seal_binding = seal.binding();
      return this->parent_->send(const_cast<uint8_t *>(buf), len, policy);
    }

    // ---- S8: MAC wire v1 framing (docs/mac-wire-format.md) -------------------

    // Section 7: a Control command carries its identity, the slot's generation
    // (per logical command, kept across retries and re-packs); nothing else does.
    static uint32_t s_v1_cmd_id(const hubframes::Down *op, uint32_t generation)
    {
      return (op->cmd_case == hubframes::DL_OPERATION ||
              op->cmd_case == hubframes::DL_SYSOP) ? generation : 0u;
    }

    bool LORAListener::v1_frame_(hubframes::Down *op, uint32_t fcnt, bool encrypt,
                                 MacFrame *out)
    {
      out->msgid = fcnt;
      out->bytes.clear();
      out->seal = mac2::FrameSeal{};
      const hubwire::Route r = op ? hubwire::routeDownlink(op->cmd_case) : hubwire::Route{};
      // On the heap: this runs on the esp_timer task too (ModeTest / MAC ping
      // frames), whose stack is 3.5 KB (StackWatch.h).
      std::vector<uint8_t> body(macwire::kMaxFrameBytes);
      size_t  body_len = 0;
      if (!r.ok || !hubwire::packDownlinkBody(op, s_v1_cmd_id(op, this->ctl().generation), body.data(),
                                              body.size(), &body_len))
      {
        ESP_LOGE(TAG, "[%s] no v1 body for command case %d", this->get_name().c_str(),
                 op ? (int) op->cmd_case : -1);
        return false;
      }
      // Seal only in session and only with a session id for the node.
      uint32_t unused_session_id = 0;
      const bool seal = encrypt && s_keys().sessionId(static_cast<mac::Addr>(this->short_address_),
                                                      &unused_session_id);
      macwire::WireHdr h;
      h.flags  = macwire::withAuth(r.ack_req ? macwire::kFlagAckReq : 0,
                                   seal ? macwire::Auth::Session : macwire::Auth::None);
      h.dst    = static_cast<uint8_t>(this->short_address_);
      h.src    = static_cast<uint8_t>(kHubAddress);
      h.subnet = static_cast<uint8_t>(this->subnet_address_);
      h.fcnt   = fcnt;
      h.port   = r.port;
      if (!hubwire::assemble(h, body.data(), body_len, &out->bytes))
      {
        ESP_LOGE(TAG, "[%s] v1 frame would not fit (%u B body)", this->get_name().c_str(),
                 (unsigned) body_len);
        return false;
      }
      return !seal || this->v1_seal_(out, macwire::headerLen(h), body_len);
    }

    // AUTH = SESSION: encrypt + tag in place; a spent FCNT asks for a session.
    bool LORAListener::v1_seal_(MacFrame *f, size_t hdr_len, size_t body_len)
    {
      const mac2::LinkSecurity::Seal sealed =
          s_sec().sealFrameV1(f->bytes.data(), hdr_len, body_len, &f->seal);
      if (sealed == mac2::LinkSecurity::Seal::MsgidSpent)
        this->requestMsgidRelogin_("seal");
      if (sealed != mac2::LinkSecurity::Seal::Ok)
      {
        ESP_LOGE(TAG, "[%s] v1 seal refused for FCNT %u (%d) — dropping the downlink",
                 this->get_name().c_str(), (unsigned) f->msgid, (int) sealed);
        f->bytes.clear();
        f->seal = mac2::FrameSeal{};
        return false;
      }
      return true;
    }

    bool LORAListener::v1_login_frame_(const LoginMsg &login, uint32_t fcnt, MacFrame *out)
    {
      out->msgid = fcnt;
      out->seal  = mac2::FrameSeal{};
      const size_t body_len = login_msg__get_packed_size(&login);
      if (body_len > macwire::kMaxFrameBytes)
        return false;
      std::vector<uint8_t> body(body_len ? body_len : 1);
      login_msg__pack(&login, body.data());
      macwire::WireHdr h;
      h.flags  = macwire::withAuth(0, macwire::Auth::KAuth);
      h.dst    = static_cast<uint8_t>(this->short_address_);
      h.src    = static_cast<uint8_t>(kHubAddress);
      h.subnet = static_cast<uint8_t>(this->subnet_address_);
      h.fcnt   = fcnt;
      h.port   = macwire::kPortSession;
      return hubwire::assemble(h, body.data(), body_len, &out->bytes) &&
             s_sec().authFrameV1(/*downlink=*/true, out->bytes.data(), out->bytes.size(), &out->seal);
    }

    void LORAListener::send_login_v1_(const LoginMsg &login, uint32_t fcnt, uint32_t base)
    {
      MacFrame f;
      if (this->v1_login_frame_(login, fcnt, &f))
        this->mac0_send_(f.bytes.data(), f.bytes.size(), TxPolicy{}, f.seal);
      else
        ESP_LOGE(TAG, "[%s] v1 LOGIN could not be framed/authenticated — not sent",
                 this->get_name().c_str());
      ESP_LOGI(TAG, "[%s] LoginMsg sent (base_nonce=0x%08x msgid=%u request_register=%d)",
               this->get_name().c_str(), (unsigned) base, (unsigned) this->seq_.txId(),
               (int) login.request_register);
    }

    bool LORAListener::v1_demote_frame_(MacFrame *out)
    {
      out->msgid = 0;
      out->seal  = mac2::FrameSeal{};
      hubframes::Down op = {};
      GridDemote demote = GRID_DEMOTE__INIT;
      op.cmd_case   = hubframes::DL_GRIDDEMOTE;
      op.griddemote = &demote;
      uint8_t body[16];
      size_t  body_len = 0;
      macwire::WireHdr h;
      h.dst       = macwire::kBroadcast;
      h.src       = static_cast<uint8_t>(kHubAddress);
      h.subnet    = macwire::kAnySubnet;
      h.port      = macwire::kPortMac0Mgmt;   // FCNT 0, AUTH = NONE (spec 4.5)
      return hubwire::packDownlinkBody(&op, 0, body, sizeof(body), &body_len) &&
             hubwire::assemble(h, body, body_len, &out->bytes);
    }

    // The uplink's shape decides its path before any key is touched (spec 4.1):
    // AUTH = SESSION walks the encrypted pipeline (verify over the raw frame,
    // then decrypt); K_AUTH is REGISTER (verified in handle_register_); NONE is
    // the plaintext bootstrap path. FLEET is never an uplink.
    std::unique_ptr<hubwire::Uplink> LORAListener::v1_receive_(uint8_t *data, size_t len)
    {
      size_t body_off = 0, body_len = 0;
      macwire::WireHdr h;
      this->v1_rx_frame_ = nullptr;
      if (!macwire::decode(data, len, &h, &body_off, &body_len))
      {
        ESP_LOGW(TAG, "%s: not a v1 frame (len %u) — dropping", this->get_name().c_str(), (unsigned) len);
        return nullptr;
      }
      this->v1_rx_hdr_   = h;
      this->v1_rx_frame_ = data;
      this->v1_rx_len_   = len;
      const macwire::Auth a = h.auth();
      std::unique_ptr<hubwire::Uplink> u(new hubwire::Uplink());
      if (a == macwire::Auth::Session)
      {
        u->sealed(h, data + body_off, body_len, data + len - macwire::kTagBytes);
        return u;
      }
      const bool session_port = h.port == macwire::kPortSession;
      if (a == macwire::Auth::Fleet || (a == macwire::Auth::KAuth) != session_port)
        return nullptr;   // REGISTER is exactly K_AUTH on PORT 1
      if (!u->parse(h, data + body_off, body_len))
        return nullptr;
      return u;
    }

    bool LORAListener::v1_verify_uplink_(EncUplink &up)
    {
      machdr::fromWireHdr(this->v1_rx_hdr_, &up.hdr);
      up.msgid = up.hdr.fcnt;
      return this->v1_rx_frame_ != nullptr &&
             s_sec().verifyUplinkV1(up.session_id, up.keys, this->v1_rx_frame_, this->v1_rx_len_) ==
                 mac2::LinkSecurity::Verify::Ok;
    }

    // The decrypted body is the message (direction, PORT) names; a session
    // frame on PORT 1 has no meaning and is dropped (REGISTER is K_AUTH).
    void LORAListener::v1_deliver_decrypted_(EncUplink &up, const uint8_t *pt, size_t len)
    {
      hubwire::Uplink u;
      if (this->v1_rx_hdr_.port == macwire::kPortSession || !u.parse(this->v1_rx_hdr_, pt, len))
      {
        ESP_LOGW(TAG, "%s: authentic v1 uplink whose body does not decode as its port's PDU",
                 this->get_name().c_str());
        return;
      }
      this->deliver_uplink_(u.view(), up.msg->header, /*authentic=*/true);
    }

    // MAC-1 commit (S3): only for an id admit_frame_ already check()ed; the
    // Sequencer persists the new mark at once (margin 1).
    void LORAListener::setRxMessageId(uint32_t msg_id)
    {
      this->seq_.commit(msg_id, esp_timer_get_time(), this->persist_());
    }

    bool LORAListener::persist_hook_(void *ctx)
    {
      auto *self = static_cast<LORAListener *>(ctx);
      // Mark exactly the snapshot save_state_ writes as persisted (node N4(a)).
      const uint32_t tx = self->seq_.txId();
      const uint32_t rx = self->seq_.rxId();
      self->save_state_(true);
      self->seq_.notePersisted(tx, rx);
      return true;
    }

    void LORAListener::send_login()
    {
      // Base-nonce policy:
      //   * Mint a fresh esp_random() ONLY for the first send of a challenge.
      //   * On every retry (pending_login_nonce_ != 0) reuse the same value.
      // Rationale: the node rate-limits CMD_LOGIN to one accepted message per
      // 5 s.  If the hub regenerates the nonce on retry, the node keeps the
      // FIRST nonce it saw (the rate-limited retry is silently dropped) while
      // the hub overwrites its s_base_nonce_map with the LAST nonce — the two
      // sides then permanently disagree and every encrypted reply fails the
      // IV check.  pending_login_nonce_ is cleared in set_response() when the
      // node's first valid reply sets login_acked_, and in enterSleep().
      uint32_t base;
      if (this->pending_login_nonce_ != 0)
      {
        base = this->pending_login_nonce_;
      }
      else
      {
        base = esp_random();
        this->pending_login_nonce_ = base;
        // A genuinely NEW challenge: whatever session keys are live belong
        // to the session this challenge supersedes. Clear them now so a
        // downlink packed before the new session-opening uplink arrives is
        // correctly dropped ("no session keys") instead of silently
        // encrypted under the old session's keys. See clearSessionKeys_()'s
        // comment for why this is NOT done on a bare retry.
        this->clearSessionKeys_();
      }

      // §4.6 rule: a new session invalidates the hub's confidence. The
      // counters reset here, the session keying changes, and the node may have
      // rebooted — none of which is compatible with still believing its
      // uplinks land in slot. It earns single-shot back from observation.
      //
      // session_changed alone did NOT achieve that. noteUplinkPlacement_ clears
      // it on the FIRST in-slot uplink, and in_slot_acks survived the login
      // untouched — so a node that had earned single-shot before re-keying got
      // it back after one observation instead of kPromotionUplinks. Earning it
      // back from observation means starting the count again.
      this->belief_.session_changed = true;
      this->belief_.in_slot_acks    = 0;
      this->have_in_slot_confirm_   = false;
      // Same reasoning as the reboot path: a measurement from the previous
      // session is not evidence about this one.
      this->belief_.phase_reported  = false;
      this->belief_.phase_samples   = 0;
      this->have_phase_report_      = false;
      this->last_gridsync_ack_us_   = 0;
      this->mbs_restart_();

      // A new session is also a new sequencing space for unauthenticated
      // frames; a high-water mark from the old one means nothing here.
      this->plaintext_hwm_      = 0;
      this->have_plaintext_hwm_ = false;

      {
        std::lock_guard<std::mutex> lock(this->session_tx_mutex_);
        this->set_counters_no_write_(0, 0);   // a new session: both directions restart
      }
      // ...so no FCNT reserved from the old count may be spent any more: the
      // new session counts up through the same numbers (P1).
      this->invalidate_fcnt_reservations_();
      this->msgid_relogin_pending_ = false;   // the msgid space restarts here
      this->dlv_forget_ids_();
      // New login challenge: the encrypted session is not confirmed until the
      // node proves it by sending a frame we can decrypt.  Until then the hub
      // sends commands in plaintext (see v1_frame_).
      this->session_confirmed_ = false;
      this->relogin_pending_   = true;

      // Store the base-nonce on the hub side before sending so it is ready to
      // validate the first encrypted response the node sends back.
      {
        s_keys().setSessionId(static_cast<mac::Addr>(this->short_address_), base);
      }

      const uint32_t fcnt = this->incrTxMessageId();

      LoginMsg login = LOGIN_MSG__INIT;
      // nonce carries the base-nonce (session id) for the CTR counter block.
      // The node stores it as its base_nonce on receipt of CMD_LOGIN.
      login.nonce = base;
      // If we have not pushed config to this node this session (config_synced_
      // is cleared on every hub boot and after a failed cover op), ask the node
      // to REGISTER.  That drives the register -> config-push -> login path so an
      // awake node that re-established via LOGIN still picks up config changes /
      // OTA flashed into the hub — without needing to reboot.  Once config is
      // pushed (config_synced_ becomes true) subsequent logins clear this flag,
      // so the handshake converges after a single register cycle.
      //
      // ...UNLESS a config push is already deferred and waiting for this very
      // session (config_push_pending_, set by handle_register_ for a node that
      // reports itself PROVISIONED). Asking that node to re-register is a
      // deadlock, and it hangs the whole link:
      //
      //   handle_register_  provisioned -> defer the push, config_synced_ stays false
      //   send_login        request_register = !config_synced_ = TRUE
      //   node              "CMD_LOGIN requests re-register" -> plaintext REGISTER,
      //                     and by design it does NOT store the nonce or ack the
      //                     login, because it expects the NEXT login to carry
      //                     request_register = false
      //   handle_register_  provisioned -> defer again, config_synced_ still false
      //   ...forever, at the node's 5 s login rate limit
      //
      // The node's expectation is correct and documented at CmdDispatcher.cpp's
      // request_register branch: a REGISTER is answered by a config push and
      // then a clean login. But the hub only pushes inline when the node reports
      // needs_config; for a provisioned node it defers to confirm_session_(),
      // which can never run, because confirming the session requires the
      // encrypted login ack that request_register just told the node to skip.
      //
      // So while a push is pending, ask for a login and nothing else. The
      // session confirms, confirm_session_() sends the deferred push encrypted
      // (the only way a provisioned node will accept it) and sets
      // config_synced_. The resume-path guarantee in confirm_session_() is
      // unaffected: it fires when config is unsynced with NO pending push,
      // where request_register is still exactly what is wanted.
      //
      // Observed at the bench: node 2 provisioned, hub freshly flashed (which
      // clears config_synced_), looping register <-> login for 8+ minutes with
      // a healthy radio at RSSI -36. No measurement can run without a session.
      login.request_register = !this->config_synced_ && !this->config_push_pending_;

      // Security review finding 7: true only on the hub's FIRST LOGIN send
      // for this node since the hub process itself started — i.e. a genuine
      // reboot. login_acked_ is exactly this: it starts false at boot/setup
      // and is never reset to false again except by this same node's own
      // reboot/sleep cycle (handle_register_, enter-sleep) or the very next
      // send_login() challenge before it fires — never by the OTHER triggers
      // that call send_login() while the node stays up (tracked-op-retry
      // exhaustion, config_sync_relogin, the REGISTER follow-up, the
      // sleep-fallback timer), which is exactly the distinction the node's
      // demote-on-LOGIN needs and did not have before.
      login.hub_rebooted = !this->login_acked_;

      // The frame's K_AUTH tag (over every raw byte, recomputed per burst copy)
      // authenticates LOGIN (spec 4.3).
      this->send_login_v1_(login, fcnt, base);
    }

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
        esp_timer_create(&args, &this->mode_test_timer_);
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
        esp_timer_create(&args, &this->drift_timer_);
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
      this->set_counters_no_write_(this->seq_.txId() + modebsup::kTxBootMargin, this->seq_.rxId());
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

      // This node's own last uplink, captured in admit_frame_ — never the
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
      // T0 is 3136 us AFTER the first chirp leaves the antenna, and earliest_us
      // is the instant lora_tx() is called. Handing the wanted T0 over as-is
      // puts the frame 3136 us late at the node — 22 % of the 14080 us guard,
      // spent before the link has done anything.
      //
      // d_tx_ramp is passed as 0 on purpose. Its only provenance is a
      // commented-out delayMicroseconds(220) (section 12.1 lists it as
      // unmeasured), and fireInstantUs takes it as a parameter precisely so a
      // placeholder cannot be mistaken for a measurement. Correcting the known
      // 3136 and leaving the unknown ~220 as residual is strictly better than
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
      this->mac0_send_(buf, len, p, seal);

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

    bool LORAListener::acquire_onboarding_()
    {
      if (this->onboarding_held_)
        return true;
      if (!s_onboarding_gate_.tryAcquire(this))
      {
        if (!this->onboarding_waiting_logged_)
        {
          this->onboarding_waiting_logged_ = true;
          ESP_LOGI(TAG, "[%s] Another node is being brought up — waiting for it to finish "
                        "(%u in line)",
                   this->get_name().c_str(), (unsigned) s_onboarding_gate_.waiting());
        }
        return false;
      }
      this->onboarding_held_             = true;
      this->onboarding_waiting_logged_   = false;
      this->onboarding_confirmed_        = false;
      this->onboarding_heard_            = false;
      this->onboarding_age_ms_           = 0;
      this->onboarding_since_confirm_ms_ = 0;
      this->set_timeout("onboarding_poll", onboarding::kPollMs, [this]() { this->onboarding_poll_(); });
      return true;
    }

    void LORAListener::release_onboarding_(const char *why)
    {
      if (!this->onboarding_held_)
        return;
      this->onboarding_held_ = false;
      this->cancel_timeout("onboarding_poll");
      s_onboarding_gate_.release(this);
      ESP_LOGI(TAG, "[%s] Onboarding done (%s, %u s) — next node may start",
               this->get_name().c_str(), why, (unsigned) (this->onboarding_age_ms_ / 1000));
    }

    void LORAListener::onboarding_poll_()
    {
      if (!this->onboarding_held_)
        return;
      this->onboarding_age_ms_ += onboarding::kPollMs;
      if (this->onboarding_confirmed_)
        this->onboarding_since_confirm_ms_ += onboarding::kPollMs;

      onboarding::HoldState s;
      s.age_ms           = this->onboarding_age_ms_;
      s.confirmed        = this->onboarding_confirmed_;
      s.since_confirm_ms = this->onboarding_since_confirm_ms_;
      s.node_heard       = this->onboarding_heard_;
      s.tx_drain_us      = (this->parent_ != nullptr) ? this->parent_->txDrainUs() : 0;
      // A schedule push is part of bringing the node up while it can still be
      // retried. Once the ladder is spent nothing more will be sent, so the
      // stale msgid (kept to recognise a late ack) must not hold the gate.
      s.schedule_outstanding = this->sched_push_outstanding_();

      switch (onboarding::assess(s))
      {
      case onboarding::Verdict::Keep:
        this->set_timeout("onboarding_poll", onboarding::kPollMs, [this]() { this->onboarding_poll_(); });
        return;
      case onboarding::Verdict::Settled:
        this->release_onboarding_("settled");
        return;
      case onboarding::Verdict::NodeSilent:
        ESP_LOGW(TAG, "[%s] Node did not answer its login — letting the other nodes go first",
                 this->get_name().c_str());
        this->release_onboarding_("node silent");
        return;
      case onboarding::Verdict::Capped:
        ESP_LOGW(TAG, "[%s] Onboarding held the gate for %u s without settling — releasing it",
                 this->get_name().c_str(), (unsigned) (this->onboarding_age_ms_ / 1000));
        this->release_onboarding_("capped");
        return;
      }
    }

    uint32_t LORAListener::ack_wait_ms_(uint32_t base_ms) const
    {
      int64_t drain_ms = (this->parent_ != nullptr) ? this->parent_->txDrainUs() / 1000 : 0;
      if (drain_ms < 0)
        drain_ms = 0;
      if (drain_ms > (int64_t) kMaxAckBacklogMs)
        drain_ms = kMaxAckBacklogMs;
      return base_ms + (uint32_t) drain_ms;
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
