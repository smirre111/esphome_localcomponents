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
      bool frame(hubframes::Down *op, uint32_t fcnt, bool encrypt, DeliveryFrame *out);
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
    };

  }  // namespace lora_tracker
}  // namespace esphome
