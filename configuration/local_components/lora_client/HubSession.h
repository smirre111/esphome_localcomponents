#pragma once
// HubSession.h -- one node's MAC-2 link on the hub (review HB10, split
// proposal 3, first part). HUB-ONLY.
//
// Owns: the node's session keys in the shared SessionKeyStore (derive a
// candidate, install, clear), the v1 downlink framing and seal, the uplink's
// shape check and the security stages of the encrypted-uplink pipeline
// (session id present, key selection, CMAC verify, CTR decrypt into its own
// buffer), and the msgid / CTR-space guard. It reaches the listener ONLY
// through SessionHost. What the listener DOES with an authentic frame (commit
// the counter, confirm the session, hand the body on) stays the listener's.
//
// Threading (docs/threading-model.md): loop task, except frame(), which the
// esp_timer bench streams call too (it touches no member state), and
// take_fcnt() / sealed_in_epoch(), which run under tx_mutex_. The key lock
// (SessionKeyStore::keys_mutex_) is taken only inside HubLinkSecurity; nothing
// here logs or makes a virtual call under it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "esphome/components/lora_client/DeliveryService.h"
#include "esphome/components/lora_client/FcntReservation.h"
#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/HubLinkSecurity.h"
#include "esphome/components/lora_client/HubWire.h"
#include "esphome/components/lora_client/MacWire.h"
#include "esphome/components/lora_client/ModeBSupervisor.h"
#include "esphome/components/lora_client/OnboardingGate.h"
#include "esphome/components/lora_client/RegisterFreshness.h"
#include "esphome/components/lora_client/SleepModel.h"
#include "esphome/components/lora_client/Sequencer.h"

namespace esphome
{
  namespace lora_tracker
  {
    // What a node's session may ask of its listener.
    class SessionHost
    {
    public:
      virtual const char *session_name() const = 0;
      // The node's short address and subnet.
      virtual uint8_t session_peer() const = 0;
      virtual uint8_t session_subnet() const = 0;
      // The Control channel's command generation (the frame's cmd_id).
      virtual uint32_t session_cmd_generation() const = 0;
      // New keys are live: reserved FCNT blocks belong to the old ones.
      virtual void session_keys_installed() = 0;
      // The keys are gone: reserved FCNT blocks and stored frames are stale.
      virtual void session_keys_cleared() = 0;
      // Start a fresh session (login, with its retry ladder).
      virtual void session_relogin() = 0;
      // The ESPHome scheduler.
      virtual void session_set_timeout(const char *name, uint32_t ms, std::function<void()> fn) = 0;
      // Write the listener's persisted state (the counters among it) now.
      virtual void session_save_state() = 0;
      virtual void session_cancel_timeout(const char *name) = 0;
      // The transmit queue's drain estimate (0 without a radio).
      virtual int64_t session_tx_drain_us() const = 0;
      // A schedule push that can still be retried is outstanding.
      virtual bool session_schedule_outstanding() const = 0;

      // ---- what the login lifecycle needs to know or do ----
      // A tracker to send through.
      virtual bool session_has_radio() const = 0;
      // The node's MAC (the REGISTER's address).
      virtual uint64_t session_mac_address() const = 0;
      // This node's index in declaration order (the login stagger).
      virtual uint32_t session_login_slot() const = 0;
      // Milliseconds until the node is predicted awake (0 = awake now).
      virtual uint32_t session_ms_until_awake() const = 0;
      // `base_ms` plus the transmit backlog.
      virtual uint32_t session_ack_wait_ms(uint32_t base_ms) const = 0;
      // Hand one frame (the LOGIN) to MAC-0.
      virtual bool session_send(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal) = 0;
      // The receive stamp (T0) of the uplink being processed.
      virtual int64_t session_rx_t0_us() const = 0;
      // Place one copy where the node listens after the uplink at t0_uplink
      // (its Class A RX1/RX2, or its Mode B mark). False: it cannot be aimed.
      virtual bool session_send_after_uplink(const uint8_t *buf, size_t len,
                                             const mac2::FrameSeal &seal, int64_t t0_uplink) = 0;
      // The burst, copy 0 placed on the node's mark recovered from the uplink at
      // t0_uplink (its REGISTER), after the queue drains. False: not placed.
      virtual bool session_send_on_register_mark(const uint8_t *buf, size_t len,
                                                 const mac2::FrameSeal &seal, int64_t t0_uplink) = 0;

      // ---- the lifecycle's events ----
      // An admitted REGISTER from this node: it is registered, heard, rebooted.
      virtual void session_register_accepted() = 0;
      // The REGISTER's configuration decision; true when configuration is pushed
      // (now or deferred to the session), which sets the login delay.
      virtual bool session_register_config(bool needs_config) = 0;
      // A new login challenge (beliefs about the old session are void).
      virtual void session_challenge_started() = 0;
      // Both counters restarted (reserved FCNTs and ack ids are stale).
      virtual void session_counters_restarted() = 0;
      // Whether this LOGIN asks the node to REGISTER.
      virtual bool session_request_register() const = 0;
      // Every confirmed session (deferred tracked op, deferred config push).
      virtual void session_on_confirmed() = 0;
      // The first confirmation since the login was last unacknowledged.
      virtual void session_on_login_acknowledged() = 0;
      // An authenticated uplink showed the node restored its counters after a
      // reboot (FCNT jump `fcnt_jump`), and the downlink counter was advanced
      // past its restored replay floor. The listener re-publishes the grid and
      // sends a TimeSync for a node that lost its RAM.
      virtual void session_node_restored(uint32_t fcnt_jump) = 0;

    protected:
      ~SessionHost() = default;
    };

    // Working state of one encrypted-uplink receive: the ordered stages of
    // the listener's handle_encrypted_ share it instead of a dozen locals.
    struct EncUplink
    {
      hubframes::Up *msg{nullptr};
      uint32_t     sender{0};
      uint32_t      session_id{0};
      mac2::KeyPair keys{};        // the keys this frame is tried under (live, or the candidate)
      mac2::KeyPair candidate{};   // an unverified candidate; destroyed unless installed
      mac::MacHdr   hdr{};         // the outer header, decoded for MAC-2
      bool          needs_candidate{false};
      uint32_t     msgid{0};
    };

    class HubSession
    {
    public:
      explicit HubSession(SessionHost &host) : host_(host) {}
      HubSession(const HubSession &) = delete;
      HubSession &operator=(const HubSession &) = delete;

      // ---- MAC-1 (stage S3): the node's frame counters ----
      // The shared mac1::Sequencer, ForwardRatchet policy -- accept only
      // rx < id <= rx + 1024, commit sets rx := id -- with persist margins of 1,
      // i.e. every id handed out and every committed uplink is written at once
      // (persist_hook_). Two-phase: the listener's admit_frame_ check()s,
      // commit_rx() commits only after MAC-2 verified (or before any session).
      const mac1::Sequencer &seq() const { return this->seq_; }
      // The next downlink FCNT (loop only: allocating persists). An exhausted
      // space hands back kMsgidCtrLimit, which the seal refuses, and asks for a
      // fresh session.
      uint32_t allocate_tx();
      // Commit an uplink FCNT the listener already check()ed.
      void     commit_rx(uint32_t msg_id);
      // Set both counters WITHOUT a write and count them as persisted (NVS
      // apply, login reset, boot margin, a reservation). Sequencer::restore with
      // the margins of 1 subtracted, exact in uint32 arithmetic (0 - 1 + 1 == 0).
      void set_counters_no_write(uint32_t tx, uint32_t rx) { this->seq_.restore(tx - 1u, rx - 1u); }
      // A new session: both directions restart (under tx_mutex_).
      void restart_counters()
      {
        std::lock_guard<std::mutex> lock(this->tx_mutex_);
        this->set_counters_no_write(0, 0);
      }
      // FCNT blocks for the esp_timer streams (FcntReservation.h).
      // reserve: loop only (one persist), n >= 1; false leaves *r invalid.
      bool     reserve_fcnts(uint32_t n, mac1::FcntReservation *r);
      // take: any task; the next FCNT of *r under this peer's current key
      // epoch, or 0 when there is none.
      uint32_t take_fcnt(mac1::FcntReservation *r);
      bool     sealed_in_epoch(const DeliveryFrame &f, const mac1::FcntReservation &r);
      // Guards the Sequencer and the FCNT reservations. The Sequencer itself is
      // loop-only (its allocation persists through ESPHome preferences); the
      // esp_timer streams take FCNTs only from a reservation, under this lock.
      // Never held while logging; keys_mutex_ is never taken inside it.
      std::mutex &tx_mutex() const { return this->tx_mutex_; }

      // ---- MAC-2 keys (SessionKeyStore, this node's entry) ----
      // Tier 3: derive K_enc/K_mac from (K_root, session_id, node_nonce,
      // hub/node addresses) and install them, destroying whatever was there
      // before only on success. Mirrors CmdDispatcher::deriveSessionKeys_().
      bool derive_keys(uint32_t session_id, uint32_t node_nonce);
      // A candidate in fresh PSA key ids, NOT installed: safe on an unverified
      // frame. The caller installs it after a verify, or destroys it.
      bool derive_candidate(uint32_t session_id, uint32_t node_nonce, mac2::KeyPair *out);
      // Install a verified candidate as the live keys (bumps the generation).
      void install_keys(uint32_t session_id, const mac2::KeyPair &keys);
      // Destroy the live keys: send_login() minted a FRESH challenge, so the
      // superseded session's keys must not (mis)encrypt a downlink before the
      // new session-opening uplink arrives. Not on a bare retry of the same nonce.
      void clear_keys();
      uint32_t generation() const;
      uint32_t stale_drops() const;
      bool     derive_keys_for_test(uint32_t session_id, uint32_t node_nonce);
      bool     ctr_encrypt_for_test(const uint8_t iv[16], const uint8_t *in, size_t len,
                                    uint8_t *out);

      // ---- MAC wire v1 (S8, docs/mac-wire-format.md) ----
      // Downlink: the v1 frame for `op` under FCNT `fcnt` -- HubMacPdu or
      // HubAppPdu behind the fixed header (hubwire::routeDownlink picks PORT and
      // ACK_REQ), sealed (AUTH = SESSION) exactly when encrypt && a session id
      // exists for the node.
      // `ack_req`: FLAGS.ACK_REQ even where the route table does not ask for it
      // (an Acked delivery of a fire-and-forget kind: ClientConfig/CoverConfig).
      bool frame(hubframes::Down *op, uint32_t fcnt, bool encrypt, DeliveryFrame *out,
                 bool ack_req = false);
      // LOGIN (PORT 1, AUTH = K_AUTH; the tag replaces LoginMsg.mic).
      bool login_frame(const ::LoginMsg &login, uint32_t fcnt, DeliveryFrame *out);
      // GridDemote (broadcast, PORT 0, AUTH = NONE, FCNT 0).
      bool demote_frame(DeliveryFrame *out);
      // Uplink: shape-check the frame and return the decoded view (owned), or
      // nullptr (dropped). Keeps the raw frame and its header for the stages below.
      std::unique_ptr<hubwire::Uplink> receive(uint8_t *data, size_t len);
      // The header of the uplink being processed (receive()).
      const macwire::WireHdr &rx_hdr() const { return this->rx_hdr_; }

      // ---- the security stages of the encrypted-uplink pipeline ----
      // Each returns false when the frame is dropped (candidate destroyed).
      bool     check_base_nonce(const EncUplink &up);
      bool     select_keys(EncUplink &up);
      bool     verify(EncUplink &up);
      // CTR-decrypt into the session's own buffer; nullptr on failure.
      uint8_t *decrypt(EncUplink &up);

      // ---- the login / session lifecycle ----
      // The state machine's flags. One struct so a test can set a precondition
      // through ONE seam (login_state_for_test) and production reads them only
      // through the accessors below.
      struct LoginState
      {
        // True once the hub has DECRYPTED a frame from this node under this
        // session: the node holds the matching base nonce. Downlink is sealed
        // only once this is set. Cleared at each new login challenge.
        bool     confirmed{false};
        bool     login_acked{false};
        // From the node's REGISTER or a new challenge until the node proves the
        // new key: TimeSync, ScheduleConfig and GridSync WAIT (confirm re-sends them).
        bool     relogin_pending{false};
        // The base nonce of the challenge in flight, reused on every retry
        // (the node keeps the FIRST LoginMsg it accepts per 5 s).
        uint32_t pending_login_nonce{0};
        uint8_t  login_retry_count{0};
        // The startup login path was initiated (never cleared by incoming
        // frames, so an NTP resync does not restart a running session).
        bool     startup_login_initiated{false};
      };
      static constexpr uint8_t  kMaxLoginRetries      = 24; // 24 × 1 h = 1 day
      static constexpr uint32_t kLoginRetryIntervalMs = 3600000; // 1 h ceiling between login retries
      static constexpr uint32_t kLoginRetryBaseMs     = 5000;    // first retry delay; doubles each retry up to the ceiling
      // Delay between receiving REGISTER and firing the login challenge.  The
      // REGISTER path first queues ClientConfig + each node's config, each sent
      // as a ~1.5-1.9 s burst.  A shorter delay (was 500 ms) queued LoginMsg on
      // top of those bursts, saturating the channel so the node's login-ack had
      // no quiet window.  4 s clears ~2 config bursts before login is sent.
      static constexpr uint32_t kRegisterToLoginDelayMs = 4000;
      // When config is skipped (provisioned node, already synced) there are no
      // config bursts to drain, so login can fire much sooner — shortening the
      // battery node's awake window.
      static constexpr uint32_t kRegisterToLoginFastMs  = 800;
      static constexpr int64_t  kRegisterRateLimitUs    = 5000000; // 5 s, matches the node's LOGIN_RATE_LIMIT_MS

      bool     confirmed() const { return this->login_.confirmed; }
      bool     login_acked() const { return this->login_.login_acked; }
      bool     relogin_pending() const { return this->login_.relogin_pending; }
      uint32_t pending_login_nonce() const { return this->login_.pending_login_nonce; }
      uint8_t  login_retry_count() const { return this->login_.login_retry_count; }
      bool     startup_login_initiated() const { return this->login_.startup_login_initiated; }
      // A node restored as registered still holds its NVS session, but the hub
      // has no keys until its first LOGIN: defer commands until then (HB5).
      void mark_relogin_pending() { this->login_.relogin_pending = true; }
      // A command went fully unacked: the session is dead (DeliveryService).
      void drop_confirmation() { this->login_.confirmed = false; }
      // setup() of a node restored as registered: a login challenge is due.
      void reset_for_boot()
      {
        this->login_.login_acked             = false;
        this->login_.login_retry_count       = 0;
        this->login_.startup_login_initiated = false;
      }
      void set_startup_login_initiated() { this->login_.startup_login_initiated = true; }
      // The node acknowledged CMD_SLEEP: sleep ends the session.
      void end_for_sleep();

      // Arm the startup login, respecting the node's sleep window and stagger.
      void schedule_startup_login();
      // A login challenge now (through the onboarding gate) and its retry ladder.
      void do_login_and_arm_retry(bool into_uplink_window = false);
      void login_retry_step();
      // The LoginMsg of a (new or retried) challenge.
      void send_login(bool into_uplink_window = false);
      // A frame from the node decrypted under this session.
      void confirm();
      // A REGISTER (bypasses the replay window; checked here, see register_admitted_).
      void handle_register(const hubframes::Up *rcv_message, const uint8_t *data, size_t len);

      LoginState &login_state_for_test() { return this->login_; }

      // ---- a node reboot, as MAC-1 sees it (review "ota-hang", 2026-10-31) ----
      // A node that boots on a resumed session restores tx = persisted + 64 (its
      // tx persist margin) and rx = persisted + 16 with that margin marked seen,
      // so it drops every hub downlink at or below its restored floor. The hub
      // must advance its downlink counter by kTxBootMargin once per node boot.
      // A reset beacon says so (the listener's note_node_reset_); when the
      // beacons are lost the counters still do: an authenticated uplink of a
      // confirmed session whose FCNT jumps at least kRebootFcntJump past the last
      // accepted one. A false positive (64 uplinks lost in a row) costs 16 ids.
      static constexpr uint32_t kRebootFcntJump = 64;   // node tx_persist_margin
      static constexpr bool rebootJump(bool confirmed, bool session_opening, uint32_t rx,
                                       uint32_t fcnt)
      {
        return confirmed && !session_opening && fcnt > rx && fcnt - rx >= kRebootFcntJump;
      }
      // An authentic uplink, BEFORE its FCNT is committed (rxId is the last one).
      void note_authentic_uplink(uint32_t fcnt, bool session_opening);
      // Advance the downlink counter past the node's restored floor, once per boot:
      // false (nothing moved) inside kRegridMinGapS of the last advance, whichever
      // path (FCNT jump or reset beacon) made it.
      bool advance_tx_for_boot();
      // Seconds since the last boot advance (0xFFFFFFFF = never).
      uint32_t s_since_boot_advance() const;

      // ---- the onboarding gate (OnboardingGate.h) ----
      // The hub has one downlink queue; bringing a node up costs ~8 frames
      // (~15 s), so two nodes started 3 s apart interleave and every ack timer
      // in both fires on frames still queued. A node may begin a login
      // challenge only while its session holds this process-wide gate.
      // True when this session holds the gate, taking it if it is free and
      // this session is first in line. False = someone else is onboarding.
      bool acquire_onboarding();
      void release_onboarding(const char *why);
      bool onboarding_held() const { return this->onboarding_held_; }
      // The settle clock starts at the session's confirmation.
      void note_onboarding_confirmed()
      {
        if (this->onboarding_held_ && !this->onboarding_confirmed_)
        {
          this->onboarding_confirmed_        = true;
          this->onboarding_since_confirm_ms_ = 0;
        }
      }
      // The node was heard (it answered its login).
      void note_onboarding_heard() { this->onboarding_heard_ = true; }
      // A session that goes away must not leave the gate held or its place in line.
      void leave_onboarding_gate() { onboarding_gate_.release(this); }
      static void reset_onboarding_gate_for_test() { onboarding_gate_.reset(); }

      // ---- msgid / CTR-space guard (framecrypto::kMsgidCtrLimit) ----
      // Starts a fresh session ONCE per exhaustion (cleared by send_login(),
      // where both counters restart).
      void     request_msgid_relogin(const char *where);
      bool     msgid_relogin_pending() const { return this->msgid_relogin_pending_; }
      uint32_t msgid_relogin_requests() const { return this->msgid_relogin_requests_; }
      void     clear_msgid_relogin() { this->msgid_relogin_pending_ = false; }

    private:
      // The Sequencer's PersistHook: the listener's save_state + notePersisted.
      static bool persist_hook_(void *ctx);
      mac1::PersistHook persist_() { return mac1::PersistHook{&HubSession::persist_hook_, this}; }
      bool seal_(DeliveryFrame *f, size_t hdr_len, size_t body_len);
      void onboarding_poll_();
      void schedule_login_retry_();
      bool register_authentic_(const uint8_t *data, size_t len);
      bool register_admitted_(const ::ClientRegister *reg, const uint8_t *data, size_t len);
      void note_keyless_uplink_();
      static constexpr int64_t kKeylessLoginGapUs = 5000000;   // the node's LOGIN rate limit
      // The receive stamp of the node's last admitted REGISTER: every LOGIN of
      // the challenge it starts is placed from it while it is fresh (0: none).
      int64_t register_t0_us_{0};
      void send_login_frame_(const ::LoginMsg &login, uint32_t fcnt, uint32_t base,
                             bool into_uplink_window);
      bool verify_uplink_(EncUplink &up);
      mac2::LinkSecurity::Verify tag_verdict_(EncUplink &up);
      void drop_candidate_(EncUplink &up);

      SessionHost &host_;
      mac1::Sequencer seq_{mac1::Config{mac1::Policy::ForwardRatchet, 1, 1}};
      mutable std::mutex tx_mutex_;
      macwire::WireHdr rx_hdr_{};
      const uint8_t   *rx_frame_{nullptr};
      size_t           rx_len_{0};
      // The decrypted body of the uplink being processed (loop only, HB8).
      uint8_t uplink_pt_[256]{};
      bool     msgid_relogin_pending_{false};
      uint32_t msgid_relogin_requests_{0};   // how many fresh sessions the guard has asked for
      static onboarding::Gate onboarding_gate_;
      LoginState login_{};
      // When a keyless uplink last re-sent the LOGIN (0 = never).
      int64_t  last_keyless_login_us_{0};
      // That uplink's T0: where the re-sent LOGIN is aimed (review rollout-3 F1).
      int64_t  keyless_t0_us_{0};
      // When the downlink counter was last advanced for a node boot (0 = never).
      int64_t  last_boot_advance_us_{0};
      // Security review finding 2: time-based REGISTER replay guard.
      int64_t  last_register_accepted_us_{0};
      // HB14: the highest register_seq this node has sent (RegisterFreshness.h).
      regfresh::Highest register_seq_{};
      bool     onboarding_held_{false};
      bool     onboarding_waiting_logged_{false};   // log the wait once, not per poll
      bool     onboarding_confirmed_{false};
      bool     onboarding_heard_{false};
      uint32_t onboarding_age_ms_{0};
      uint32_t onboarding_since_confirm_ms_{0};
    };

  }  // namespace lora_tracker
}  // namespace esphome
