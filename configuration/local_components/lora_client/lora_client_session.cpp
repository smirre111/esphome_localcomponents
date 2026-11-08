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
                 (unsigned) this->session_.seq().rxId(), (unsigned) this->short_address_);
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
        if (rcv_message->header && this->session_.seq().check(rcv_message->header->msgid))
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
                   (unsigned) this->session_.seq().rxId());
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
      // §4.6: the same stamp answers a second question — did this uplink land
      // where the grid says this node transmits?
      this->link_.commit_uplink_stamp(rx.ticket.t0_us, rx.unc_us);
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
      if (this->ctl_cmd_.deferred_until_login)
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
        ESP_LOGI(TAG, "[%s] Session confirmed — sending the deferred config push (acked)",
                 this->get_name().c_str());
        const bool notify = this->pending_register_notice_;
        this->pending_register_notice_ = false;
        this->push_config_acked_(notify);
      }
    }

    // ---- the login / session lifecycle (HubSession, review HB10) ----

    void HubSession::schedule_startup_login()
    {
      this->host_.session_cancel_timeout("login_startup");
      this->host_.session_cancel_timeout("login_retry");

      // Per-node stagger: SLOT × sleepmodel::kLoginStaggerMs, slot being this node's
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
      uint32_t stagger_ms = this->host_.session_login_slot() * sleepmodel::kLoginStaggerMs;

      uint32_t delay_ms = this->host_.session_ms_until_awake();
      if (delay_ms == 0)
      {
        // Node is in its wake window — wait 5 s for radio init + per-node stagger.
        uint32_t total_ms = sleepmodel::kBootMarginMs + stagger_ms;
        ESP_LOGI(TAG, "[%s] Node awake — login challenge in %u s (5 s radio-init + %u s stagger)",
                 this->host_.session_name(), total_ms / 1000, stagger_ms / 1000);
        this->host_.session_set_timeout("login_startup", total_ms,
                                        [this]() { this->do_login_and_arm_retry(); });
      }
      else
      {
        // Add sleepmodel::kBootMarginMs beyond the calculated wake time so that REGISTER
        // (sent by the node ~1-3 s after deep-sleep wake-up) almost always arrives
        // before this timer fires and cancels it.  The startup timer is then purely
        // a fallback for lost REGISTER packets.
        uint32_t total_ms = delay_ms + sleepmodel::kBootMarginMs + stagger_ms;
        ESP_LOGI(TAG, "[%s] Node asleep — login deferred %u s (%u s sleep + %u s boot margin + %u s stagger)",
                 this->host_.session_name(), total_ms / 1000,
                 delay_ms / 1000, sleepmodel::kBootMarginMs / 1000, stagger_ms / 1000);
        this->host_.session_set_timeout("login_startup", total_ms,
                                        [this]() { this->do_login_and_arm_retry(); });
      }
    }

    void HubSession::do_login_and_arm_retry(bool into_uplink_window)
    {
      // Do NOT bail on login_acked here.  After a hub reboot the node may have
      // already sent a status message (setting login_acked=true via the incoming-
      // message path) before this timer fired.  We still need to send LoginMsg so
      // that the base-nonce map is repopulated (it is cleared on hub
      // reboot as a file-scope static and is never persisted to flash).
      //
      // One node's onboarding at a time. The downlink is a single queue at ~1.85 s
      // a frame; a second node's login started behind the first's config, time
      // and schedule pushes sits in that queue while every ack timer runs. Checked
      // before the log so a wait is one line, not one per poll.
      if (this->host_.session_has_radio() && !this->acquire_onboarding())
      {
        // The aim survives the wait (round 5: a keyless re-send waited for the
        // gate and went out as a plain burst); a stamp too old by then is refused.
        this->host_.session_set_timeout("login_startup", onboarding::kPollMs, [this, into_uplink_window]() {
          this->do_login_and_arm_retry(into_uplink_window);
        });
        return;
      }
      ESP_LOGI(TAG, "[%s] Login challenge firing (parent=%s)",
               this->host_.session_name(), !this->host_.session_has_radio() ? "NULL" : "ok");
      if (!this->host_.session_has_radio())
      {
        ESP_LOGE(TAG, "[%s] parent_ is null — cannot send login (register_client() missing set_parent?)",
                 this->host_.session_name());
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
      this->login_.login_acked = false;
      // A re-send aimed at a deaf node's check-in uplink continues the running
      // challenge: it does NOT restart the ladder, or every check-in would buy
      // another round of fast retries (review 2026-11-08 R4: 103 LoginMsgs to
      // node 1 in 2 h).
      if (!into_uplink_window)
        this->login_.login_retry_count = 0;
      // Whatever started this challenge, the boot-time one is now redundant (R4:
      // an uplink at boot + 2.7 s started one, the startup timer a second 2.3 s
      // later).
      this->host_.session_cancel_timeout("login_startup");   // the boot timer

      this->send_login(into_uplink_window);   // resets frame counters to 0 + sends LoginMsg

      // Retry every hour; give up after kMaxLoginRetries (24 h).  One-shot,
      // self-re-arming — NOT set_interval (whose random initial phase fired the
      // first "hourly" retry within ~0.6 s, clashing with the register bursts).
      this->host_.session_cancel_timeout("login_retry");
      this->schedule_login_retry_();
    }

    void HubSession::schedule_login_retry_()
    {
      // Self-re-arming ONE-SHOT set_timeout with exponential backoff: fast early
      // retries (a transient handshake loss recovers in seconds) growing to the
      // 1 h ceiling.  delay = base << retry_count, capped at kLoginRetryIntervalMs.
      // The first kFastLoginRetries at the base delay (the phase sweep), then
      // doubling from there.
      const uint32_t n     = this->login_.login_retry_count;
      const uint32_t steps = (n < kFastLoginRetries) ? 0 : n - kFastLoginRetries + 1;
      const uint32_t shift = (steps < 20) ? steps : 20;
      uint64_t delay = static_cast<uint64_t>(kLoginRetryBaseMs) << shift;
      if (delay > kLoginRetryIntervalMs)
        delay = kLoginRetryIntervalMs;
      // The backoff counts from when the login is ON THE AIR, not from when it
      // was queued behind another node's bursts.
      delay = this->host_.session_ack_wait_ms(static_cast<uint32_t>(delay));
      // Never retry earlier than the node can hear it. A sleeping node cannot
      // answer, so a burst sent before its wake is pure airtime and hub TX
      // energy — 68 such retries were observed in one log before this gate.
      //
      // The delay is stretched, not replaced: the backoff still governs how
      // often we try once the node IS awake.
      const uint32_t until_awake = this->host_.session_ms_until_awake();
      if (until_awake > delay)
      {
        ESP_LOGD(TAG, "[%s] Login retry deferred %u s — node asleep until then",
                 this->host_.session_name(), until_awake / 1000);
        delay = until_awake;
      }

      this->host_.session_set_timeout("login_retry", static_cast<uint32_t>(delay),
                                      [this]() { this->login_retry_step(); });
    }

    void HubSession::login_retry_step()
    {
      if (this->login_.login_acked)
        return; // acked already; one-shot, nothing to re-arm
      // A retry is a login challenge like any other: it may not start while
      // another node is being brought up. The attempt is NOT counted and the
      // backoff is not advanced — it has not been made yet.
      if (this->host_.session_has_radio() && !this->acquire_onboarding())
      {
        this->host_.session_set_timeout("login_retry", onboarding::kPollMs,
                                        [this]() { this->login_retry_step(); });
        return;
      }
      if (++this->login_.login_retry_count >= kMaxLoginRetries)
      {
        ESP_LOGE(TAG, "[%s] Login not acknowledged after %u retries — giving up",
                 this->host_.session_name(), static_cast<unsigned>(kMaxLoginRetries));
        this->release_onboarding("gave up");
        return;
      }
      ESP_LOGW(TAG, "[%s] Login not acknowledged — retry %u/%u",
               this->host_.session_name(),
               static_cast<unsigned>(this->login_.login_retry_count),
               static_cast<unsigned>(kMaxLoginRetries));
      this->send_login();
      this->schedule_login_retry_(); // re-arm the next one-shot
    }

    // A REGISTER is accepted before any address or FCNT filter, so a node whose
    // address has just changed can still get through. Its proof is the frame's
    // K_AUTH tag over every raw byte (spec 4.3).
    bool HubSession::register_authentic_(const uint8_t *data, size_t len)
    {
      return s_sec().verifyAuthFrameV1(/*downlink=*/false, data, len);
    }

    // The REGISTER's own checks, in order: addressed to this node, authentic
    // (K_AUTH tag), fresh (register_seq, HB14), not a rapid repeat (rate limit).
    bool HubSession::register_admitted_(const ::ClientRegister *reg, const uint8_t *data, size_t len)
    {
        ESP_LOGD(TAG, "Received register command for MAC address: 0x%012" PRIx64, (uint64_t) reg->mac_addr);
        ESP_LOGD(TAG, "This clients' MAC address: 0x%012" PRIx64, (uint64_t) this->host_.session_mac_address());
        if (reg->mac_addr != this->host_.session_mac_address())
        {
          // Every REGISTER reaches every listener; all but the addressed one land
          // here. That is routing, not a fault, and at ERROR it read as one — it
          // was logged twice per handshake and hid the real errors around it.
          ESP_LOGD(TAG, "%s: REGISTER is for another node's MAC — ignoring", this->host_.session_name());
          return false;
        }

        // Tier 3 (mac-separation-implementation-plan.md section 2(b), step
        // 7): verify REGISTER's MIC before any state change — this used to
        // be accepted purely on an address match, before any filter at all,
        // so a forged plaintext REGISTER naming a real node's MAC could drop
        // that node's existing session outright.
        if (!this->register_authentic_(data, len))
        {
          ESP_LOGW(TAG, "%s: REGISTER failed MIC verification — dropping", this->host_.session_name());
          return false;
        }

        // HB14: a replayed REGISTER carries a register_seq the node has already
        // used. Checked before the rate limit, so a genuine frame the limit
        // drops still raises the bar for its own replay.
        if (!this->register_seq_.admit(reg->register_seq))
        {
          ESP_LOGW(TAG, "%s: REGISTER replayed (register_seq %u, highest seen %u) — dropping",
                   this->host_.session_name(), (unsigned) reg->register_seq,
                   (unsigned) this->register_seq_.highest());
          return false;
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
        const int64_t now_us = esp_timer_get_time();
        if (this->last_register_accepted_us_ != 0 &&
            (now_us - this->last_register_accepted_us_) < kRegisterRateLimitUs)
        {
          ESP_LOGW(TAG, "%s: REGISTER rate-limited (%lld ms after the last one) — dropping",
                   this->host_.session_name(),
                   (long long) ((now_us - this->last_register_accepted_us_) / 1000));
          return false;
        }
        this->last_register_accepted_us_ = now_us;
        return true;
    }

    void HubSession::handle_register(const hubframes::Up *rcv_message, const uint8_t *data, size_t len)
    {
        const ClientRegister *reg = rcv_message->register_;
        if (!this->register_admitted_(reg, data, len))
          return;

        ESP_LOGI(TAG, "%s, Registered with LORA server", this->host_.session_name());
        // Where the node listens: it sends its REGISTER at its own mark + the
        // uplink offset when it is on a grid (rollout-4, see send_login_frame_).
        this->register_t0_us_      = this->host_.session_rx_t0_us();
        this->register_login_sends_ = 0;
        this->host_.session_register_accepted();
        const bool push_config = this->host_.session_register_config(reg->needs_config);

        // Re-negotiate nonce and reset frame counters after (re-)registration.
        // Cancel any pending login timers from a previous boot/sleep cycle, then
        // arm a short delay so ClientConfig + node configs are transmitted first.
        this->login_.login_acked             = false;
        this->login_.login_retry_count       = 0;
        this->login_.relogin_pending         = true;  // the node restarted: its old session is gone
        this->login_.pending_login_nonce     = 0;     // node just (re-)registered → mint fresh
        // The node rebooted (its REGISTER says so): the old session's keys and
        // any stored ciphertext are void on both sides, so drop them now rather
        // than at the next LOGIN challenge.
        this->clear_keys();
        this->login_.startup_login_initiated = true;  // prevent NTP callback from cancelling this timer
        this->host_.session_cancel_timeout("login_startup");
        this->host_.session_cancel_timeout("login_retry");

        // The startup timer (schedule_startup_login) fires 5 s AFTER the node's
        // expected wake time so that REGISTER (sent by the node ~1-3 s after boot)
        // almost always arrives first and cancels the timer via cancel_timeout above.
        // If REGISTER arrives while the timer is still pending we still schedule the
        // login here — the node's 5-second rate-limit window is wide enough to
        // absorb the rare case where both paths reach do_login_and_arm_retry() at
        // nearly the same time (the second LoginMsg is simply rate-limited on the node).
        // When config was pushed, kRegisterToLoginDelayMs lets the ClientConfig +
        // node-config bursts drain first so LoginMsg is not queued on top of them.
        // When config was skipped there is nothing to drain, so login fast.
        const uint32_t login_delay_ms = push_config ? kRegisterToLoginDelayMs
                                                     : kRegisterToLoginFastMs;
        ESP_LOGI(TAG, "[%s] REGISTER received — login in %u ms",
                 this->host_.session_name(), (unsigned)login_delay_ms);
        this->host_.session_set_timeout("login_startup", login_delay_ms, [this]() {
          this->do_login_and_arm_retry();
        });
    }

    // A successful decrypt proves the node holds the matching base nonce, so
    // the encrypted session is confirmed both ways. Everything the hub defers
    // until it can encrypt downlink safely hangs off this one moment (the
    // listener's session_on_confirmed / session_on_login_acknowledged).
    //
    // Deliberately NOT triggered by a plaintext frame bumping the msgid — that
    // does not prove the node holds the session key, and treating it as proof
    // once left a node that missed the LoginMsg unable to decrypt commands.
    void HubSession::confirm()
    {
      this->login_.confirmed       = true;
      this->login_.relogin_pending = false;
      this->register_t0_us_        = 0;   // later challenges have no REGISTER to aim from

      // The settle clock for the onboarding gate starts here: the pushes that
      // follow a confirmation are queued over the next ~2 s.
      this->note_onboarding_confirmed();

      // Security review finding 1 (the deferred tracked op) and the deferred
      // config push: on EVERY confirm that finds one waiting.
      this->host_.session_on_confirmed();

      // U2 (Tier-1 interim mitigation, round 12): clear pending_login_nonce
      // unconditionally on every confirm, not only on the first ack. Three
      // call paths reach send_login() while login_acked is already true
      // (config_sync_relogin, the node-LOGIN path, the missing-nonce path),
      // and each of those previously skipped this clear, so the next
      // send_login() reused the still-pending nonce under a reset msgid —
      // real base-nonce (keystream) reuse. This alone does not fully close the
      // issue (send_login's own unconditional tx/rx reset is the deeper
      // bug; Tier 3's session redesign is the real fix) but removes this
      // specific repeat-clear gap cheaply in the meantime.
      this->login_.pending_login_nonce = 0;
      if (!this->login_.login_acked)
      {
        this->login_.login_acked       = true;
        this->login_.login_retry_count = 0;
        this->host_.session_cancel_timeout("login_retry");
        ESP_LOGI(TAG, "[%s] Login acknowledged by node (encrypted session confirmed)",
                 this->host_.session_name());
        this->host_.session_on_login_acknowledged();
      }
    }

    void HubSession::send_login(bool into_uplink_window)
    {
      // Base-nonce policy:
      //   * Mint a fresh esp_random() ONLY for the first send of a challenge.
      //   * On every retry (pending_login_nonce != 0) reuse the same value.
      // Rationale: the node rate-limits CMD_LOGIN to one accepted message per
      // 5 s.  If the hub regenerated the nonce on retry, the node would keep the
      // FIRST nonce it saw (the rate-limited retry is silently dropped) while
      // the hub's session id would be the LAST one — the two
      // sides then permanently disagree and every encrypted reply fails the
      // IV check.  pending_login_nonce is cleared when the node's first valid
      // reply confirms the session (confirm()), and at sleep.
      uint32_t base;
      if (this->login_.pending_login_nonce != 0)
      {
        base = this->login_.pending_login_nonce;
      }
      else
      {
        base = esp_random();
        this->login_.pending_login_nonce = base;
        this->sweep_base_us_ = 0;   // a new challenge sweeps the round afresh
        this->sweep_sends_   = 0;
        // A genuinely NEW challenge: whatever session keys are live belong
        // to the session this challenge supersedes. Clear them now so a
        // downlink packed before the new session-opening uplink arrives is
        // correctly dropped ("no session keys") instead of silently
        // encrypted under the old session's keys. See clear_keys()'s
        // comment for why this is NOT done on a bare retry.
        this->clear_keys();
      }

      // A new session invalidates the hub's confidence and its sequencing
      // state for unauthenticated frames (the listener's reactions).
      this->host_.session_challenge_started();

      this->restart_counters();   // a new session: both directions restart
      // ...so no FCNT reserved from the old count may be spent any more (P1),
      // and ack ids on record would alias the new msgid space.
      this->host_.session_counters_restarted();
      this->clear_msgid_relogin();   // the msgid space restarts here
      // New login challenge: the encrypted session is not confirmed until the
      // node proves it by sending a frame we can decrypt.  Until then the hub
      // sends commands in plaintext (see frame()).
      this->login_.confirmed       = false;
      this->login_.relogin_pending = true;

      // Store the base-nonce on the hub side before sending so it is ready to
      // validate the first encrypted response the node sends back.
      {
        s_keys().setSessionId(static_cast<mac::Addr>(this->host_.session_peer()), base);
      }

      const uint32_t fcnt = this->allocate_tx();

      LoginMsg login = LOGIN_MSG__INIT;
      // nonce carries the base-nonce (session id) for the CTR counter block.
      // The node stores it as its base_nonce on receipt of CMD_LOGIN.
      login.nonce = base;
      // Ask the node to REGISTER when this hub boot has not pushed its config
      // yet -- but not while a push is already waiting for this session (the
      // listener's session_request_register explains the deadlock).
      login.request_register = this->host_.session_request_register();

      // Security review finding 7: true only on the hub's FIRST LOGIN send
      // for this node since the hub process itself started — i.e. a genuine
      // reboot. login_acked is exactly this: it starts false at boot/setup
      // and is never reset to false again except by this same node's own
      // reboot/sleep cycle (handle_register, enter-sleep) or the very next
      // send_login() challenge before it fires — never by the OTHER triggers
      // that call send_login() while the node stays up (tracked-op-retry
      // exhaustion, config_sync_relogin, the REGISTER follow-up, the
      // sleep-fallback timer), which is exactly the distinction the node's
      // demote-on-LOGIN needs and did not have before.
      login.hub_rebooted = !this->login_.login_acked;

      // The frame's K_AUTH tag (over every raw byte, recomputed per burst copy)
      // authenticates LOGIN (spec 4.3).
      this->send_login_frame_(login, fcnt, base, into_uplink_window);
    }

    // The v1 LOGIN of send_login: frame, send, log.
    void HubSession::send_login_frame_(const LoginMsg &login, uint32_t fcnt, uint32_t base,
                                       bool into_uplink_window)
    {
      DeliveryFrame f;
      if (!this->login_frame(login, fcnt, &f))
        ESP_LOGE(TAG, "[%s] v1 LOGIN could not be framed/authenticated — not sent",
                 this->host_.session_name());
      // Aimed where the node listens after the uplink that triggered it (review
      // rollout-3 F1: a Mode B node hears an unplaced burst ~1/3 of the time);
      // the burst when it cannot be aimed. The retry ladder's LOGINs stay bursts.
      else if (into_uplink_window &&
               this->host_.session_send_after_uplink(f.bytes.data(), f.bytes.size(), f.seal,
                                                     this->keyless_t0_us_))
        ESP_LOGI(TAG, "[%s] LoginMsg aimed at the node's receive window after its uplink",
                 this->host_.session_name());
      // After a REGISTER, the burst with copy 0 on the node's mark (rollout-4: a
      // Mode B node missed every unplaced burst of a REGISTER cycle, which all
      // met its one window per round at the same phase).
      // (A zero stamp -- no REGISTER yet -- is refused there.) Alternating with
      // the classic unplaced burst: a node whose mark the stamp does not give
      // (hardware check 2026-11-05: node 1) is still reached the old way.
      else if (this->next_register_send_placed_() &&
               this->host_.session_send_on_register_mark(f.bytes.data(), f.bytes.size(), f.seal,
                                                         this->register_t0_us_))
        ESP_LOGI(TAG, "[%s] LoginMsg burst placed on the node's mark from its REGISTER",
                 this->host_.session_name());
      // No REGISTER of this challenge to aim from: the phase sweep. (A REGISTER's
      // odd sends stay the classic unplaced burst, as the user chose.)
      else if (this->register_t0_us_ == 0 && this->send_swept_(f))
        ESP_LOGI(TAG, "[%s] LoginMsg burst %u of the phase sweep", this->host_.session_name(),
                 (unsigned) this->sweep_sends_);
      else
        this->host_.session_send(f.bytes.data(), f.bytes.size(), f.seal);
      ESP_LOGI(TAG, "[%s] LoginMsg sent (base_nonce=0x%08x msgid=%u request_register=%d)",
               this->host_.session_name(), (unsigned) base, (unsigned) this->seq().txId(),
               (int) login.request_register);
    }

    // The k-th burst of the sweep at the first one's phase + kLoginSweepOffsetsUs.
    bool HubSession::send_swept_(const DeliveryFrame &f)
    {
      const int64_t phase =
          (this->sweep_base_us_ == 0)
              ? 0
              : this->sweep_base_us_ + kLoginSweepOffsetsUs[this->sweep_sends_ % kLoginSweepBursts];
      const int64_t fire = this->host_.session_send_burst_at(f.bytes.data(), f.bytes.size(), f.seal, phase);
      if (fire == 0)
        return false;
      if (this->sweep_base_us_ == 0)
        this->sweep_base_us_ = fire;
      ++this->sweep_sends_;
      return true;
    }

    // Placed, unplaced, placed, ... over the LOGINs of a REGISTER's challenge.
    bool HubSession::next_register_send_placed_()
    {
      return (this->register_login_sends_++ % 2u) == 0;
    }

    void HubSession::note_authentic_uplink(uint32_t fcnt, bool session_opening)
    {
      const uint32_t rx = this->seq_.rxId();
      if (!rebootJump(this->login_.confirmed, session_opening, rx, fcnt))
        return;
      if (this->advance_tx_for_boot())
        this->host_.session_node_restored(fcnt - rx);
    }

    // The node is transmitting (and listens after each uplink) while the hub
    // holds no keys because its LOGIN has not landed: re-send it now instead of
    // at the next back-off step, which a stale sleep record could stretch by
    // hours (review rollout-3 D2). Rate-limited (the node accepts one LoginMsg
    // per 5 s); not "heard" -- the frame is unauthenticated, and forging one
    // costs the attacker a LOGIN burst per kKeylessLoginGapUs at most.
    void HubSession::note_keyless_uplink_()
    {
      if (!this->login_.relogin_pending)
        return;
      const int64_t now_us = esp_timer_get_time();
      if (this->last_keyless_login_us_ != 0 && now_us - this->last_keyless_login_us_ < kKeylessLoginGapUs)
        return;
      ESP_LOGI(TAG, "[%s] Node transmits without the new session — re-sending its LOGIN now",
               this->host_.session_name());
      this->login_from_uplink_();
    }

    // A LOGIN aimed where the node listens after the uplink being processed,
    // through the onboarding gate and the ladder; at most one per
    // kKeylessLoginGapUs (the node's LOGIN rate limit; the frame is
    // unauthenticated). Also the "no base nonce" path after a hub boot.
    void HubSession::login_from_uplink_()
    {
      const int64_t now_us = esp_timer_get_time();
      if (this->last_keyless_login_us_ != 0 && now_us - this->last_keyless_login_us_ < kKeylessLoginGapUs)
        return;
      this->last_keyless_login_us_ = (now_us > 0) ? now_us : 1;
      this->keyless_t0_us_         = this->host_.session_rx_t0_us();
      this->host_.session_cancel_timeout("login_retry");
      this->host_.session_set_timeout("login_keyless", 0,
                                      [this]() { this->do_login_and_arm_retry(/*into_uplink_window=*/true); });
    }

    bool HubSession::advance_tx_for_boot()
    {
      if (this->s_since_boot_advance() < modebsup::kRegridMinGapS)
        return false;
      const int64_t now_us = esp_timer_get_time();
      this->last_boot_advance_us_ = (now_us > 0) ? now_us : 1;
      this->set_counters_no_write(this->seq_.txId() + modebsup::kTxBootMargin, this->seq_.rxId());
      return true;
    }

    uint32_t HubSession::s_since_boot_advance() const
    {
      return (this->last_boot_advance_us_ == 0)
                 ? 0xFFFFFFFFu
                 : (uint32_t) ((esp_timer_get_time() - this->last_boot_advance_us_) / 1000000);
    }

    // The node acknowledged CMD_SLEEP: sleep ends the session; mint fresh on wake.
    void HubSession::end_for_sleep()
    {
      this->login_.login_acked         = false;
      this->login_.login_retry_count   = 0;
      this->login_.pending_login_nonce = 0;   // sleep ends the session; mint fresh on wake
      this->host_.session_cancel_timeout("login_retry");   // ("login_startup" is replaced by the arming below)
    }

    // ---- the listener's side of the session lifecycle (SessionHost events) ----

    bool LORAListener::session_has_radio() const { return this->parent_ != nullptr; }
    uint64_t LORAListener::session_mac_address() const { return this->address_uint64_; }
    uint32_t LORAListener::session_login_slot() const
    {
      return static_cast<uint32_t>(this->login_slot_);
    }
    uint32_t LORAListener::session_ms_until_awake() const { return this->ms_until_node_awake_(); }
    uint32_t LORAListener::session_ack_wait_ms(uint32_t base_ms) const
    {
      return this->ack_wait_ms_(base_ms);
    }
    int64_t LORAListener::session_rx_t0_us() const { return this->rx_ticket_.ticket.t0_us; }
    bool LORAListener::session_send_after_uplink(const uint8_t *buf, size_t len,
                                                 const mac2::FrameSeal &seal, int64_t t0_uplink)
    {
      if (this->link_.send_into_class_a_window_from(buf, len, seal, t0_uplink) == ClassAPlacement::Placed)
        return true;
      if (this->timed_mode_enabled_ && this->grid_aligned_)
        return this->link_.send_on_uplink_mark(buf, len, seal, t0_uplink);
      return false;   // Mode A: the window free-runs, only the burst covers it
    }
    int64_t LORAListener::session_send_burst_at(const uint8_t *buf, size_t len,
                                                const mac2::FrameSeal &seal, int64_t phase_fire_us)
    {
      return this->link_.send_burst_at_phase(buf, len, seal, phase_fire_us,
                                             esp_timer_get_time() + this->session_tx_drain_us());
    }
    // Whatever the hub believes about the node's mode: placing costs at most a
    // round's wait and the copies still cover a free-running window.
    bool LORAListener::session_send_on_register_mark(const uint8_t *buf, size_t len,
                                                     const mac2::FrameSeal &seal, int64_t t0_uplink)
    {
      return this->link_.send_burst_on_uplink_mark(buf, len, seal, t0_uplink,
                                                   esp_timer_get_time() + this->session_tx_drain_us());
    }
    bool LORAListener::session_send(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal)
    {
      return this->mac0_send_(buf, len, TxPolicy{}, seal);
    }

    // An admitted REGISTER from THIS node's MAC.
    void LORAListener::session_register_accepted()
    {
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
        // The node's last phase report described a grid it held before it
        // restarted. It is not evidence about the node in front of us now.
        this->link_.note_node_rebooted();
    }

    // The REGISTER's configuration decision. True when configuration is pushed
    // (now or deferred to the session), which sets the login delay.
    bool LORAListener::session_register_config(bool needs_config)
    {
        // Only (re)push configuration when the node reports it is unprovisioned
        // (needs_config), or when this hub boot has not yet pushed to the node
        // (!config_synced_ → delivers config changes flashed into the hub once
        // per boot).  A provisioned node waking from deep sleep keeps its
        // persisted config, so both are false and we skip the two ~1.5 s config
        // bursts and go straight to login — saving awake radio time on battery.
        const bool needs_cfg    = needs_config;
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
          // Fire-and-forget in the clear (the node has no session to ack under):
          // NOT recorded as in sync. The first confirmed session re-sends it
          // encrypted and acked (review rollout-3 D1).
          this->config_push_pending_     = true;
          this->pending_register_notice_ = true;
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
        return push_config;
    }

    // A new login challenge.
    void LORAListener::session_challenge_started()
    {
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
      // Same reasoning as the reboot path: a measurement from the previous
      // session is not evidence about this one.
      this->link_.note_session_changed();
      this->last_gridsync_ack_us_   = 0;
      this->mbs_restart_();

      // A new session is also a new sequencing space for unauthenticated
      // frames; a high-water mark from the old one means nothing here.
      this->plaintext_hwm_      = 0;
      this->have_plaintext_hwm_ = false;
      // A LOGIN is going out: no other node's Mode B warm-up starts over it.
      mbs_arbiter_().holdOff(this->mbs_now_s_(), modebsup::kLoginHoldOffS);
    }

    // The node rebooted on its resumed session and the hub learnt it from the
    // counters alone (its reset beacon was lost). The downlink counter is already
    // past the node's restored floor; what remains is what a reset beacon would
    // have triggered for a node that lost its RAM: confidence, the grid, the clock.
    // Not for an automatic-mode node or one waking from a SLEEP the hub commanded:
    // their deep-sleep wakes restore the same way, and their beacons (with the
    // node's retry ladder) already drive the TimeSync and its weekly throttle.
    void LORAListener::session_node_restored(uint32_t fcnt_jump)
    {
      ESP_LOGW(TAG, "[%s] Node restored its counters (uplink FCNT +%u, no reset beacon) — "
                    "advanced tx_message_id by %u to %u",
               this->get_name().c_str(), (unsigned) fcnt_jump,
               (unsigned) modebsup::kTxBootMargin, (unsigned) this->session_.seq().txId());
      const bool woke_from_commanded_sleep =
          this->last_sleep_epoch_ != 0 && this->last_heard_epoch_ < this->last_sleep_epoch_;
      if (this->auto_mode_ || woke_from_commanded_sleep)
        return;
      this->link_.note_node_rebooted();
      if (this->regrid_node_("Node rebooted (counters restored) without a beacon"))
        this->mbs_restart_();
      this->set_timeout("timesync_push", 750, [this]() { this->send_timesync(); });
    }

    // Both counters restarted for the new session.
    void LORAListener::session_counters_restarted()
    {
      // No FCNT reserved from the old count may be spent any more: the new
      // session counts up through the same numbers (P1).
      this->invalidate_fcnt_reservations_();
      this->dlv_forget_ids_();
    }

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
    bool LORAListener::session_request_register() const
    {
      return !this->config_synced_ && !this->config_push_pending_;
    }

    // Every confirmed session.
    void LORAListener::session_on_confirmed()
    {
      // Security review finding 1: send the tracked op that send_cover_
      // operation()/send_tracked_sysop_() deferred instead of packing
      // plaintext. Unlike the config/TimeSync/GridSync deferrals below, this
      // is NOT gated on !login_acked — a tracked op can be deferred by any
      // relogin, not just the hub's first one this process, so it must fire
      // on every confirm that finds one waiting.
      this->send_deferred_tracked_op_();

      // The deferred config push from handle_register_. This is the first
      // moment it can be encrypted, and a provisioned node will not accept it
      // any other way. config_synced_ is set HERE rather than at the point the
      // push was decided, so a push that never happened is not recorded as one
      // that did.
      this->send_deferred_config_push_();
    }

    // The FIRST confirmation since the login was last unacknowledged.
    void LORAListener::session_on_login_acknowledged()
    {
        this->acked_since_confirm_ = false;

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

    // The resume-path config-sync guarantee of confirm_session_ (moved out
    // unchanged): a fresh login when config is still unsynced this boot.
    void LORAListener::schedule_config_sync_relogin_()
    {
      // An acked config push already on its way settles it (review rollout-3 D1).
      if (!this->config_synced_ && !this->setup_in_progress_())
      {
        ESP_LOGI(TAG, "[%s] Session resumed without config sync this boot — scheduling re-login to push config",
                 this->get_name().c_str());
        this->set_timeout("config_sync_relogin", 500, [this]() {
          // Same reasoning as finding 9's handle_encrypted_ fix: go
          // through do_login_and_arm_retry_(), not a bare send_login(),
          // so this also respects acquire_onboarding_() and arms a retry
          // instead of being a one-shot that silently does nothing if
          // this particular LoginMsg is lost.
          if (!this->config_synced_ && !this->setup_in_progress_())
            this->do_login_and_arm_retry_();
        });
      }
    }

    // Tier 3 (mac-separation-implementation-plan.md section 2(b)): derive
    // this listener's K_enc/K_mac from (K_root, session_id, node_nonce,
    // hub/node addresses) — never installed here. Mirrors
    // CmdDispatcher::deriveSessionKeys_() on the node. MAC-2: LinkSecurity.
    bool HubSession::derive_candidate(uint32_t session_id, uint32_t node_nonce,
                                                  mac2::KeyPair *out)
    {
      return s_sec().deriveCandidate(static_cast<mac::Addr>(kHubAddress),
                                     static_cast<mac::Addr>(this->host_.session_peer()), session_id,
                                     node_nonce, out);
    }

    // Invariant I3 (c): the store installs under its key lock, racing the
    // tracker task's retag safely. Logged after the lock is released.
    void HubSession::install_keys(uint32_t session_id, const mac2::KeyPair &keys)
    {
      const mac::Addr peer = static_cast<mac::Addr>(this->host_.session_peer());
      s_keys().install(peer, session_id, keys);
      this->host_.session_keys_installed();
      ESP_LOGI(TAG, "[%s] Session keys installed (generation=%u)", this->host_.session_name(),
               (unsigned) s_keys().generation(peer));
    }

    bool HubSession::derive_keys(uint32_t session_id, uint32_t node_nonce)
    {
      mac2::KeyPair keys;
      if (!this->derive_candidate(session_id, node_nonce, &keys))
        return false;
      this->install_keys(session_id, keys);
      return true;
    }

    void HubSession::clear_keys()
    {
      s_keys().clearKeys(static_cast<mac::Addr>(this->host_.session_peer()));
      // A reserved FCNT block belongs to the keys just destroyed (P1).
      this->host_.session_keys_cleared();
    }

    uint32_t HubSession::generation() const
    {
      return s_keys().generation(static_cast<mac::Addr>(this->host_.session_peer()));
    }

    bool HubSession::ctr_encrypt_for_test(const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
    {
      return s_sec().ctrEncryptForTest(static_cast<mac::Addr>(this->host_.session_peer()), iv, in, len, out);
    }

    bool LORAListener::deriveRootKeyForTest(uint8_t out[16])
    {
      return mac2::LinkSecurity::deriveRootKey(out);
    }

    bool HubSession::derive_keys_for_test(uint32_t session_id, uint32_t node_nonce)
    {
      s_keys().setSessionId(static_cast<mac::Addr>(this->host_.session_peer()), session_id);
      return this->derive_keys(session_id, node_nonce);
    }

    uint32_t HubSession::stale_drops() const
    {
      return s_keys().staleDrops(static_cast<mac::Addr>(this->host_.session_peer()));
    }

    // A candidate not yet installed must be destroyed on every exit that is not
    // the install call at the end of a successful verify+decrypt.
    void HubSession::drop_candidate_(EncUplink &up)
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
    bool HubSession::check_base_nonce(const EncUplink &up)
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
      this->login_from_uplink_();   // aimed at this uplink, once per 5 s
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
    bool HubSession::select_keys(EncUplink &up)
    {
      // uplink_check_base_nonce_ ran first: the sender has a session id.
      s_keys().sessionId(static_cast<mac::Addr>(up.sender), &up.session_id);
      const mac::Addr self = static_cast<mac::Addr>(this->host_.session_peer());
      up.keys = s_keys().liveKeys(self);
      const hubframes::Hdr *hdr = up.msg->header;
      const bool is_session_opening = hdr && hdr->sessionnonce != 0;
      up.needs_candidate =
          is_session_opening &&
          (!up.keys.hasEnc() || s_keys().installedSessionId(self) != up.session_id);
      if (!up.needs_candidate)
        return true;
      if (!this->derive_candidate(up.session_id, hdr->sessionnonce, &up.candidate))
      {
        ESP_LOGE(TAG, "Session key candidate derivation failed for peer %u", up.sender);
        return false;
      }
      up.keys = up.candidate;
      return true;
    }

    // The tag check alone: CMAC over the raw frame (spec 4.1).
    mac2::LinkSecurity::Verify HubSession::tag_verdict_(EncUplink &up)
    {
      return this->verify_uplink_(up) ? mac2::LinkSecurity::Verify::Ok
                                         : mac2::LinkSecurity::Verify::BadTag;
    }

    // Authenticate the frame: keys present, CMAC verifies, and the authentic
    // msgid is inside the CTR space. Every failure destroys the candidate.
    bool HubSession::verify(EncUplink &up)
    {
      if (!up.keys.complete())
      {
        ESP_LOGW(TAG, "No session keys derived yet for peer %u — dropping encrypted uplink", up.sender);
        this->drop_candidate_(up);
        this->note_keyless_uplink_();
        return false;
      }

      const mac2::LinkSecurity::Verify verdict = this->tag_verdict_(up);
      if (verdict == mac2::LinkSecurity::Verify::NoMem)
      {
        ESP_LOGE(TAG, "Memory allocation failed for MAC input");
        this->drop_candidate_(up);
        return false;
      }
      if (verdict != mac2::LinkSecurity::Verify::Ok)
      {
        ESP_LOGE(TAG, "Encrypt-then-CMAC verification failed");
        this->drop_candidate_(up);
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
                 this->host_.session_name(), (unsigned) up.msgid);
        this->drop_candidate_(up);
        this->request_msgid_relogin("uplink");
        return false;
      }
      return true;
    }

    // CTR-decrypt the verified ciphertext into the listener's own buffer (no
    // heap on the receive path, HB8). Returns it, or nullptr (candidate
    // destroyed) on failure. Loop task only.
    uint8_t *HubSession::decrypt(EncUplink &up)
    {
      const hubframes::Sealed *enc = up.msg->encrypted;
      const size_t cipher_len = enc->ciphertext.len;
      if (cipher_len > sizeof(this->uplink_pt_))
      {
        ESP_LOGE(TAG, "%u B ciphertext does not fit a LoRa frame — dropped", (unsigned) cipher_len);
        this->drop_candidate_(up);
        return nullptr;
      }
      if (!s_sec().decryptUplink(up.hdr, up.session_id, up.keys, enc->ciphertext.data, cipher_len,
                                 this->uplink_pt_))
      {
        ESP_LOGE(TAG, "CTR decryption failed despite a verified CMAC — this should never happen");
        this->drop_candidate_(up);
        return nullptr;
      }
      return this->uplink_pt_;
    }

    // The frame is authentic and decrypted: commit it, install the candidate
    // session (if any), prove the node alive, confirm the session, and hand the
    // inner payload on. `plaintext` is uplink_pt_.
    void LORAListener::uplink_accept_(EncUplink &up, uint8_t *plaintext)
    {
      // The node's counters may show it rebooted (its beacons can be lost): read
      // before the commit moves the mark.
      this->session_.note_authentic_uplink(up.msgid, up.needs_candidate);

      // The tag verified: this frame is the node's, so its msgid is real and
      // the replay counter may finally move.
      this->commit_rx_msgid_(up.msg);

      // The candidate (if one was derived) just proved itself by verifying
      // THIS frame — install it as the live session now, replacing
      // whatever was there before. This is the only path that installs a
      // newly-derived candidate; every early return above destroyed it
      // instead.
      if (up.needs_candidate)
        this->session_.install_keys(up.session_id, up.candidate);

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

      if (!this->session_.check_base_nonce(up))
        return;
      if (!this->session_.select_keys(up))
        return;
      if (!this->session_.verify(up))
        return;
      uint8_t *plaintext = this->session_.decrypt(up);
      if (plaintext == nullptr)
        return;
      this->uplink_accept_(up, plaintext);
    }

    // A v1 frame is shape-checked and turned into the envelope the rest of
    // set_response walks; anything else (a frame whose VER is not 0x01) is dropped.
    std::unique_ptr<hubwire::Uplink> LORAListener::unpack_uplink_(uint8_t *data, size_t len)
    {
      return this->session_.receive(data, len);
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
      if (!this->session_.confirmed())
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

    // ---- SessionHost: what the node's MAC-2 link may ask of the listener ----
    const char *LORAListener::session_name() const { return this->get_name().c_str(); }
    uint8_t LORAListener::session_peer() const { return this->short_address_; }
    uint8_t LORAListener::session_subnet() const { return this->subnet_address_; }
    uint32_t LORAListener::session_cmd_generation() const { return this->ctl_cmd_.generation; }
    void LORAListener::session_keys_installed() { this->invalidate_fcnt_reservations_(); }
    void LORAListener::session_keys_cleared()
    {
      this->invalidate_fcnt_reservations_();

      // Invariant I3 (a): the Control channel's stored frame (pack once, retries
      // re-send the bytes) is sealed under the keys just destroyed, so it must
      // never be re-sent: the retry re-packs or, for an at-most-once command,
      // is abandoned (DeliveryService::resend_).
      this->dlv_.drop_stored_frame(CH_CONTROL);   // the delivery service's stored frame
    }
    void LORAListener::session_relogin() { this->do_login_and_arm_retry_(); }
    void LORAListener::session_save_state() { this->save_state_(true); }
    void LORAListener::session_cancel_timeout(const char *name) { this->cancel_timeout(name); }
    int64_t LORAListener::session_tx_drain_us() const
    {
      return (this->parent_ != nullptr) ? this->parent_->txDrainUs() : 0;
    }
    bool LORAListener::session_gridsync_outstanding() const { return this->mac().awaiting_ack; }
    void LORAListener::session_set_timeout(const char *name, uint32_t ms, std::function<void()> fn)
    {
      this->set_timeout(name, ms, std::move(fn));
    }

    uint32_t HubSession::allocate_tx()
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
        std::lock_guard<std::mutex> lock(this->tx_mutex_);
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
        this->request_msgid_relogin("allocate");
      return val;
    }

    void HubSession::request_msgid_relogin(const char *where)
    {
      // Once per exhaustion: the flag is cleared in send_login(), which is where
      // both msgid counters restart. Until then every refusal is silent.
      if (this->msgid_relogin_pending_)
        return;
      this->msgid_relogin_pending_ = true;
      ++this->msgid_relogin_requests_;
      ESP_LOGE(TAG, "[%s] msgid space exhausted (%s): the CTR keystream would repeat — "
                    "starting a fresh session", this->host_.session_name(), where);
      // Deferred, not inline: callers sit inside frame building and the
      // admission path, and a login from there would re-enter them.
      this->host_.session_set_timeout("msgid_relogin", 0, [this]() { this->host_.session_relogin(); });
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

    // FLAGS.ACK_REQ: the route's, or forced by an Acked delivery (review rollout-3 D1).
    static uint8_t ackReqFlag(bool route_acked, bool forced)
    {
      return (route_acked || forced) ? macwire::kFlagAckReq : 0;
    }

    bool HubSession::frame(hubframes::Down *op, uint32_t fcnt, bool encrypt,
                                 DeliveryFrame *out, bool ack_req)
    {
      out->msgid = fcnt;
      out->bytes.clear();
      out->seal = mac2::FrameSeal{};
      const hubwire::Route r = op ? hubwire::routeDownlink(op->cmd_case) : hubwire::Route{};
      // On the heap: this runs on the esp_timer task too (ModeTest / MAC ping
      // frames), whose stack is 3.5 KB (StackWatch.h).
      std::vector<uint8_t> body(macwire::kMaxFrameBytes);
      size_t  body_len = 0;
      if (!r.ok || !hubwire::packDownlinkBody(op, s_v1_cmd_id(op, this->host_.session_cmd_generation()), body.data(),
                                              body.size(), &body_len))
      {
        ESP_LOGE(TAG, "[%s] no v1 body for command case %d", this->host_.session_name(),
                 op ? (int) op->cmd_case : -1);
        return false;
      }
      // Seal only in session and only with a session id for the node.
      uint32_t unused_session_id = 0;
      const bool seal = encrypt && s_keys().sessionId(static_cast<mac::Addr>(this->host_.session_peer()),
                                                      &unused_session_id);
      macwire::WireHdr h;
      // A route's ACK_REQ on SEALED frames only: the plaintext bootstrap
      // ClientConfig/CoverConfig to an unprovisioned node stays ACK_REQ = 0
      // (node portmap::ackReqExpected = acked && sealed). Every other acked
      // route is only ever sent sealed.
      h.flags  = macwire::withAuth(ackReqFlag(r.ack_req && seal, ack_req),
                                   seal ? macwire::Auth::Session : macwire::Auth::None);
      h.dst    = static_cast<uint8_t>(this->host_.session_peer());
      h.src    = static_cast<uint8_t>(kHubAddress);
      h.subnet = static_cast<uint8_t>(this->host_.session_subnet());
      h.fcnt   = fcnt;
      h.port   = r.port;
      if (!hubwire::assemble(h, body.data(), body_len, &out->bytes))
      {
        ESP_LOGE(TAG, "[%s] v1 frame would not fit (%u B body)", this->host_.session_name(),
                 (unsigned) body_len);
        return false;
      }
      return !seal || this->seal_(out, macwire::headerLen(h), body_len);
    }

    // AUTH = SESSION: encrypt + tag in place; a spent FCNT asks for a session.
    bool HubSession::seal_(DeliveryFrame *f, size_t hdr_len, size_t body_len)
    {
      const mac2::LinkSecurity::Seal sealed =
          s_sec().sealFrameV1(f->bytes.data(), hdr_len, body_len, &f->seal);
      if (sealed == mac2::LinkSecurity::Seal::MsgidSpent)
        this->request_msgid_relogin("seal");
      if (sealed != mac2::LinkSecurity::Seal::Ok)
      {
        ESP_LOGE(TAG, "[%s] v1 seal refused for FCNT %u (%d) — dropping the downlink",
                 this->host_.session_name(), (unsigned) f->msgid, (int) sealed);
        f->bytes.clear();
        f->seal = mac2::FrameSeal{};
        return false;
      }
      return true;
    }

    bool HubSession::login_frame(const LoginMsg &login, uint32_t fcnt, DeliveryFrame *out)
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
      h.dst    = static_cast<uint8_t>(this->host_.session_peer());
      h.src    = static_cast<uint8_t>(kHubAddress);
      h.subnet = static_cast<uint8_t>(this->host_.session_subnet());
      h.fcnt   = fcnt;
      h.port   = macwire::kPortSession;
      return hubwire::assemble(h, body.data(), body_len, &out->bytes) &&
             s_sec().authFrameV1(/*downlink=*/true, out->bytes.data(), out->bytes.size(), &out->seal);
    }

    bool HubSession::demote_frame(DeliveryFrame *out)
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
    std::unique_ptr<hubwire::Uplink> HubSession::receive(uint8_t *data, size_t len)
    {
      size_t body_off = 0, body_len = 0;
      macwire::WireHdr h;
      this->rx_frame_ = nullptr;
      if (!macwire::decode(data, len, &h, &body_off, &body_len))
      {
        ESP_LOGW(TAG, "%s: not a v1 frame (len %u) — dropping", this->host_.session_name(), (unsigned) len);
        return nullptr;
      }
      this->rx_hdr_   = h;
      this->rx_frame_ = data;
      this->rx_len_   = len;
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

    bool HubSession::verify_uplink_(EncUplink &up)
    {
      machdr::fromWireHdr(this->rx_hdr_, &up.hdr);
      up.msgid = up.hdr.fcnt;
      return this->rx_frame_ != nullptr &&
             s_sec().verifyUplinkV1(up.session_id, up.keys, this->rx_frame_, this->rx_len_) ==
                 mac2::LinkSecurity::Verify::Ok;
    }

    // The decrypted body is the message (direction, PORT) names; a session
    // frame on PORT 1 has no meaning and is dropped (REGISTER is K_AUTH).
    void LORAListener::v1_deliver_decrypted_(EncUplink &up, const uint8_t *pt, size_t len)
    {
      hubwire::Uplink u;
      if (this->session_.rx_hdr().port == macwire::kPortSession ||
          !u.parse(this->session_.rx_hdr(), pt, len))
      {
        ESP_LOGW(TAG, "%s: authentic v1 uplink whose body does not decode as its port's PDU",
                 this->get_name().c_str());
        return;
      }
      this->deliver_uplink_(u.view(), up.msg->header, /*authentic=*/true);
    }

    // MAC-1 commit (S3): only for an id admit_frame_ already check()ed; the
    // Sequencer persists the new mark at once (margin 1).
    void HubSession::commit_rx(uint32_t msg_id)
    {
      this->seq_.commit(msg_id, esp_timer_get_time(), this->persist_());
    }

    bool HubSession::persist_hook_(void *ctx)
    {
      auto *self = static_cast<HubSession *>(ctx);
      // Mark exactly the snapshot save_state_ writes as persisted (node N4(a)).
      const uint32_t tx = self->seq_.txId();
      const uint32_t rx = self->seq_.rxId();
      self->host_.session_save_state();
      self->seq_.notePersisted(tx, rx);
      return true;
    }

    bool HubSession::reserve_fcnts(uint32_t n, mac1::FcntReservation *r)
    {
      const mac::Addr peer = static_cast<mac::Addr>(this->host_.session_peer());
      const uint32_t first = (n > 0) ? this->allocate_tx() : 0;   // one persist
      if (first == 0 || first >= framecrypto::kMsgidCtrLimit)
      {
        std::lock_guard<std::mutex> lock(this->tx_mutex_);
        r->invalidate();
        return false;
      }
      uint32_t last = first + (n - 1);
      if (last < first || last >= framecrypto::kMsgidCtrLimit)
        last = framecrypto::kMsgidCtrLimit - 1;
      const uint32_t epoch = s_keys().keyEpoch(peer);   // keys_mutex_ never nests inside ours
      {
        std::lock_guard<std::mutex> lock(this->tx_mutex_);
        this->set_counters_no_write(last, this->seq_.rxId());   // skip the block
        r->grant(first, last, epoch);
      }
      this->host_.session_save_state();   // ...and persist its end
      return true;
    }

    uint32_t HubSession::take_fcnt(mac1::FcntReservation *r)
    {
      const uint32_t epoch = s_keys().keyEpoch(static_cast<mac::Addr>(this->host_.session_peer()));
      std::lock_guard<std::mutex> lock(this->tx_mutex_);
      return r->take(epoch);
    }

    // A frame sealed from a reserved FCNT must carry the reservation's key
    // epoch: otherwise the keys moved between take() and the seal and the
    // ciphertext must never reach the air.
    bool HubSession::sealed_in_epoch(const DeliveryFrame &f, const mac1::FcntReservation &r)
    {
      std::lock_guard<std::mutex> lock(this->tx_mutex_);
      return f.seal.sealed && f.seal.key_epoch == r.keyEpoch();
    }

    onboarding::Gate HubSession::onboarding_gate_;

    bool HubSession::acquire_onboarding()
    {
      if (this->onboarding_held_)
        return true;
      if (!onboarding_gate_.tryAcquire(this))
      {
        if (!this->onboarding_waiting_logged_)
        {
          this->onboarding_waiting_logged_ = true;
          ESP_LOGI(TAG, "[%s] Another node is being brought up — waiting for it to finish "
                        "(%u in line)",
                   this->host_.session_name(), (unsigned) onboarding_gate_.waiting());
        }
        return false;
      }
      this->onboarding_held_             = true;
      this->onboarding_waiting_logged_   = false;
      this->onboarding_confirmed_        = false;
      this->onboarding_heard_            = false;
      this->onboarding_age_ms_           = 0;
      this->onboarding_since_confirm_ms_ = 0;
      this->host_.session_set_timeout("onboarding_poll", onboarding::kPollMs, [this]() { this->onboarding_poll_(); });
      return true;
    }

    void HubSession::release_onboarding(const char *why)
    {
      if (!this->onboarding_held_)
        return;
      this->onboarding_held_ = false;
      this->host_.session_cancel_timeout("onboarding_poll");
      onboarding_gate_.release(this);
      ESP_LOGI(TAG, "[%s] Onboarding done (%s, %u s) — next node may start",
               this->host_.session_name(), why, (unsigned) (this->onboarding_age_ms_ / 1000));
    }

    void HubSession::onboarding_poll_()
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
      s.tx_drain_us      = this->host_.session_tx_drain_us();
      s.gridsync_outstanding = this->host_.session_gridsync_outstanding();
      s.repeat_silent        = this->onboarding_was_silent_;
      s.others_waiting       = onboarding_gate_.waiting() > 0;

      const onboarding::Verdict v = onboarding::assess(s);
      // Every hold that ends records whether it ended silent (R3).
      if (v != onboarding::Verdict::Keep)
        this->onboarding_was_silent_ = (v == onboarding::Verdict::NodeSilent);
      switch (v)
      {
      case onboarding::Verdict::Keep:
        this->host_.session_set_timeout("onboarding_poll", onboarding::kPollMs, [this]() { this->onboarding_poll_(); });
        return;
      case onboarding::Verdict::Settled:
        this->release_onboarding("settled");
        return;
      case onboarding::Verdict::NodeSilent:
        ESP_LOGW(TAG, "[%s] Node did not answer its login — letting the other nodes go first",
                 this->host_.session_name());
        this->release_onboarding("node silent");
        return;
      case onboarding::Verdict::Capped:
        ESP_LOGW(TAG, "[%s] Onboarding held the gate for %u s without settling — releasing it",
                 this->host_.session_name(), (unsigned) (this->onboarding_age_ms_ / 1000));
        this->release_onboarding("capped");
        return;
      }
    }

  }  // namespace lora_tracker
}  // namespace esphome
