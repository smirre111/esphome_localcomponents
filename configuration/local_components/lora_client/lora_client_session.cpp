// lora_client_session.cpp -- the listener's link and session side: the
// receive pipeline (set_response -> admit_frame_ -> handle_encrypted_ /
// plaintext bootstrap -> commit), MAC-1 counter allocation and commit, the
// MAC-2 session lifecycle (REGISTER, LOGIN, session keys, confirmation, the
// onboarding gate) and MAC wire v1 framing. Stage 5 split of lora_client.cpp,
// moved verbatim. Loop task only, except take_reserved_fcnt_'s callers
// (lora_client_bench.cpp; docs/threading-model.md).
#include "lora_client_internal.h"

namespace esphome
{
  namespace lora_tracker
  {

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

    // A REGISTER is accepted before any address or FCNT filter, so a node whose
    // address has just changed can still get through. Its proof is the frame's
    // K_AUTH tag over every raw byte (spec 4.3).
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
        ESP_LOGD(TAG, "Received register command for MAC address: 0x%012" PRIx64, (uint64_t) reg->mac_addr);
        ESP_LOGD(TAG, "This clients' MAC address: 0x%012" PRIx64, (uint64_t) this->address_uint64_);
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
        // The node rebooted (its REGISTER says so): the old session's keys and
        // any stored ciphertext are void on both sides, so drop them now rather
        // than at the next LOGIN challenge.
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
        ESP_LOGV(TAG, "Received message with ID %u from address %u (my rx id %u, my address %u)",
                 (unsigned) rcv_message->header->msgid, (unsigned) rcv_message->header->senderaddress,
                 (unsigned) this->seq_.rxId(), (unsigned) this->short_address_);
      } else {
        ESP_LOGD(TAG, "Received message without header");
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
          ESP_LOGE(TAG, "%s, duplicate or old FCNT %u, ignoring (rx id %u)", this->get_name().c_str(),
                   (unsigned) (rcv_message->header ? rcv_message->header->msgid : 0u),
                   (unsigned) this->seq_.rxId());
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

      // Invariant I3 (a): the Control channel's stored frame (pack once, retries
      // re-send the bytes) is sealed under the keys just destroyed, so it must
      // never be re-sent: the retry re-packs or, for an at-most-once command,
      // is abandoned (dlv_resend_).
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
    // This matters most after a HUB reboot: the session ids live in RAM
    // (SessionKeyStore),
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

    // A session-opening uplink (SESSION_OPENING, carrying the NODE_NONCE)
    // yields a CANDIDATE K_enc/K_mac in fresh PSA key ids, not yet live. It is
    // used to verify THIS frame only and installed only if that verify
    // succeeds, so a forged node nonce costs one derivation and never pins
    // attacker-chosen keys. A copy of an opening uplink whose session id the
    // live keys already belong to (SessionKeyStore::installedSessionId) uses
    // the live keys: no new import, no generation bump. This is what lets the
    // hub follow a node through any number of re-logins in one uptime.
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
      ESP_LOGD(TAG, "Received encrypted payload ciphertext=%u bytes", (unsigned) enc->ciphertext.len);

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
        ESP_LOGW(TAG, "%s: uplink is not a v1 frame this listener can decode — dropped",
                 this->get_name().c_str());
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

    bool LORAListener::mac0_send_(const uint8_t *buf, size_t len, TxPolicy policy,
                                  const mac2::FrameSeal &seal)
    {
      // A copy is sealed over its raw bytes (spec section 5), never by decoding
      // a header.
      policy.sealer       = seal.sealed ? &mac2::LinkSecurity::copySealerV1 : nullptr;
      policy.seal_binding = seal.binding();
      return this->parent_->send(const_cast<uint8_t *>(buf), len, policy);
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
      // 5 s.  If the hub regenerated the nonce on retry, the node would keep the
      // FIRST nonce it saw (the rate-limited retry is silently dropped) while
      // the hub's session id would be the LAST one — the two
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
        this->mac1_restart_session_();   // a new session: both directions restart
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

  }  // namespace lora_tracker
}  // namespace esphome
