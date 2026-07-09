#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esp_random.h"
#include <algorithm>
#include <map>
#include <cstring>
#include "psa/crypto.h"

// ---------------------------------------------------------------------------
// Crypto constants — kept identical to BlindsESP/main/CmdDispatcher.cpp
// so both sides of the link derive the same key and use the same parameters.
// ---------------------------------------------------------------------------
static constexpr const char    *kLoRaAesGcmKey  = "LoRaKey1";
static constexpr size_t         kAesGcmKeyBytes = 16;
static constexpr size_t         kAesGcmIvBytes  = 12;
static constexpr size_t         kAesGcmTagBytes = 8;  // truncated AES-GCM tag (slim on-air)
// PSA algorithm carrying the shortened tag length — key policy + encrypt/decrypt.
// MUST match the node (CmdDispatcher.cpp).
#define LORA_GCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, kAesGcmTagBytes)

// PSA key slot — imported once at setup(), reused for every decrypt call.
static psa_key_id_t s_aes_gcm_key_id = PSA_KEY_ID_NULL;

// File-scope encryption state, keyed by peer short address.
// Only the base nonce is tracked per peer; the AES-GCM frame counter is the
// same value as the protobuf LoraHeader.msgid — one unified counter per
// direction, used for both replay protection and nonce derivation.
static std::map<uint32_t, uint32_t> s_base_nonce_map;

// ---------------------------------------------------------------------------
// Derive the 16-byte AES-GCM key via SHA-256("LoRaKey1")[0:16].
// Mirrors derive_aes_gcm_key() in BlindsESP CmdDispatcher.cpp.
// ---------------------------------------------------------------------------
static bool s_derive_aes_gcm_key(uint8_t key_out[kAesGcmKeyBytes])
{
  uint8_t hash[32];
  size_t  hash_len = 0;
  psa_status_t status = psa_hash_compute(
      PSA_ALG_SHA_256,
      reinterpret_cast<const uint8_t *>(kLoRaAesGcmKey),
      strlen(kLoRaAesGcmKey),
      hash, sizeof(hash), &hash_len);
  if (status != PSA_SUCCESS)
    return false;
  memcpy(key_out, hash, kAesGcmKeyBytes);
  return true;
}

// ---------------------------------------------------------------------------
// Import the AES-GCM key into PSA once and store the key ID.
// Mirrors init_psa_key() in BlindsESP CmdDispatcher.cpp.
// ---------------------------------------------------------------------------
static bool s_init_psa_gcm_key()
{
  if (s_aes_gcm_key_id != PSA_KEY_ID_NULL)
    return true; // already imported

  uint8_t key_material[kAesGcmKeyBytes];
  if (!s_derive_aes_gcm_key(key_material))
    return false;

  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attrs, LORA_GCM_ALG);
  psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attrs, kAesGcmKeyBytes * 8);
  psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);

  psa_status_t status = psa_import_key(&attrs, key_material, kAesGcmKeyBytes,
                                       &s_aes_gcm_key_id);
  memset(key_material, 0, sizeof(key_material)); // zero key material from stack
  if (status != PSA_SUCCESS)
  {
    s_aes_gcm_key_id = PSA_KEY_ID_NULL;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Downlink (hub->node) encryption helpers.  Mirror the node's
// pack_response_message()/derive_gcm_nonce() so the two directions stay wire
// compatible.  Uplink decryption in set_response() keeps its own inline lambdas.
// ---------------------------------------------------------------------------

// Direction bit for the GCM nonce counter — set on downlink so hub->node and
// node->hub never reuse an IV under the shared per-peer base nonce.  MUST match
// kDownlinkNonceFlag on the node (CmdDispatcher.cpp).
static constexpr uint64_t kDownlinkNonceFlag = (1ULL << 63);

static void s_u32_be(uint32_t v, uint8_t *b)
{
  for (int i = 3; i >= 0; --i) { b[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}
static void s_u64_be(uint64_t v, uint8_t *b)
{
  for (int i = 7; i >= 0; --i) { b[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

static bool s_build_header_aad(const LoraHeader *h, uint8_t *aad, size_t *aad_len)
{
  if (!h || !aad || !aad_len) return false;
  s_u32_be(h->destaddress,   aad);
  s_u32_be(h->destsubnet,    aad + 4);
  s_u32_be(h->senderaddress, aad + 8);
  s_u32_be(h->msgid,         aad + 12);
  // encrypted header field removed — AAD is the 16-byte 4-field header.
  *aad_len = 16;
  return true;
}

// IV = base_nonce(peer)[4 BE] || (counter | direction)[8 BE].
static bool s_derive_gcm_nonce(uint32_t peer_address, uint64_t frame_counter, uint8_t nonce_out[12])
{
  auto it = s_base_nonce_map.find(peer_address);
  if (it == s_base_nonce_map.end()) return false;
  s_u32_be(it->second, nonce_out);
  s_u64_be(frame_counter, nonce_out + 4);
  return true;
}

static bool s_encrypt_payload_gcm(const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                                  const uint8_t *plain, size_t plain_len,
                                  uint8_t *cipher_out, uint8_t *tag_out, size_t tag_len)
{
  if (s_aes_gcm_key_id == PSA_KEY_ID_NULL && !s_init_psa_gcm_key())
    return false;

  // PSA writes ciphertext||tag concatenated into one output buffer.
  size_t   ct_cap = plain_len + tag_len;
  uint8_t *ct     = static_cast<uint8_t *>(malloc(ct_cap));
  if (!ct)
    return false;

  size_t       ct_len = 0;
  psa_status_t st     = psa_aead_encrypt(
      s_aes_gcm_key_id, LORA_GCM_ALG,
      nonce, kAesGcmIvBytes,
      aad,   aad_len,
      plain, plain_len,
      ct,    ct_cap, &ct_len);
  if (st != PSA_SUCCESS || ct_len != plain_len + tag_len)
  {
    if (st != PSA_SUCCESS)
      ESP_LOGE("lora_client", "psa_aead_encrypt failed: %d", static_cast<int>(st));
    free(ct);
    return false;
  }
  memcpy(cipher_out, ct, plain_len);
  memcpy(tag_out,    ct + plain_len, tag_len);
  free(ct);
  return true;
}

// Pack a downlink operation, encrypting it into an EncryptedPayload-wrapped
// LoraClientOperationMessage when a session (base nonce) exists for the dest
// node.  Falls back to a plaintext pack otherwise (e.g. pre-login bootstrap:
// LoginMsg / BaseNonceExchange / ClientConfig are sent via their own paths and
// stay plaintext regardless).  Returns malloc'd wire bytes in *out/*out_len;
// caller frees with free().
static bool s_pack_operation_message(LoraClientOperationMessage *plain, bool encrypt,
                                     uint8_t **out, size_t *out_len)
{
  if (!plain || !plain->header || !out || !out_len)
    return false;

  const uint32_t dest = plain->header->destaddress;

  // Send plaintext when the caller hasn't confirmed the session (encrypt==false)
  // or no base nonce exists yet.  This keeps a node that never established
  // encryption controllable and avoids emitting ciphertext it cannot decrypt.
  if (!encrypt || s_base_nonce_map.find(dest) == s_base_nonce_map.end())
  {
    *out_len = lora_client_operation_message__get_packed_size(plain);
    *out     = static_cast<uint8_t *>(malloc(*out_len));
    if (!*out)
      return false;
    lora_client_operation_message__pack(plain, *out);
    return true;
  }

  // Encrypt the payload ONLY — the inner message's header is redundant (the node
  // uses the plaintext outer header), so strip it before packing.
  LoraClientOperationMessage inner_msg = *plain;
  inner_msg.header = nullptr;
  size_t   inner_len = lora_client_operation_message__get_packed_size(&inner_msg);
  uint8_t *inner     = static_cast<uint8_t *>(malloc(inner_len));
  if (!inner)
    return false;
  lora_client_operation_message__pack(&inner_msg, inner);

  // Outer header mirrors the inner addressing + msgid.  AAD covers only these
  // four header fields, so the tracker's burst re-stamping of burstIndex/
  // burstCount does not invalidate the GCM tag.  Encryption is signalled by the
  // `encrypted` oneof case, not a header flag.
  LoraHeader outer      = LORA_HEADER__INIT;
  outer.destaddress     = plain->header->destaddress;
  outer.destsubnet      = plain->header->destsubnet;
  outer.senderaddress   = plain->header->senderaddress;
  outer.msgid           = plain->header->msgid;

  uint8_t  iv[12];
  uint64_t counter = static_cast<uint64_t>(outer.msgid) | kDownlinkNonceFlag;
  uint8_t  aad[20];
  size_t   aad_len = 0;
  uint8_t *cipher  = static_cast<uint8_t *>(malloc(inner_len));
  uint8_t  tag[kAesGcmTagBytes];
  if (!cipher ||
      !s_derive_gcm_nonce(dest, counter, iv) ||
      !s_build_header_aad(&outer, aad, &aad_len) ||
      !s_encrypt_payload_gcm(iv, aad, aad_len, inner, inner_len, cipher, tag, kAesGcmTagBytes))
  {
    free(inner);
    free(cipher);
    return false;
  }
  free(inner);

  EncryptedPayload enc     = ENCRYPTED_PAYLOAD__INIT;
  enc.tag.data             = tag;
  enc.tag.len              = kAesGcmTagBytes;
  enc.ciphertext.data      = cipher;
  enc.ciphertext.len       = inner_len;

  LoraClientOperationMessage wrapped = LORA_CLIENT_OPERATION_MESSAGE__INIT;
  wrapped.header    = &outer;
  wrapped.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED;
  wrapped.encrypted = &enc;

  *out_len = lora_client_operation_message__get_packed_size(&wrapped);
  *out     = static_cast<uint8_t *>(malloc(*out_len));
  if (!*out)
  {
    free(cipher);
    return false;
  }
  lora_client_operation_message__pack(&wrapped, *out);
  free(cipher);
  return true;
}

namespace esphome
{
  namespace lora_tracker
  {
    static const char *const TAG = "lora_client";

    void LORAListener::dump_config()
    {

      ESP_LOGCONFIG(TAG,
                    "  Short address: %d\n"
                    "  Subnet address: %d\n"
                    "  Sleep duration: %d",
                    this->short_address_, this->subnet_address_, this->sleep_duration_);
    }

    void LORAListener::setup()
    {
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
                 (unsigned)this->frame_counter_.rx_message_id,
                 (unsigned)this->frame_counter_.tx_message_id,
                 this->registered_ ? "yes" : "no");
      }
      else
      {
        ESP_LOGW(TAG, "[%s] NVS restore failed or version mismatch (expected v%u) — "
                 "starting fresh, waiting for REGISTER",
                 this->get_name().c_str(), (unsigned)kRestoreStateVersion);
        this->frame_counter_.rx_message_id = 0;
        this->frame_counter_.tx_message_id = 0;
        this->registered_ = false;
      }

      // Initialise PSA Crypto and import the AES-GCM key once.
      // psa_crypto_init() is idempotent — safe to call from multiple instances.
      psa_status_t psa_ret = psa_crypto_init();
      if (psa_ret != PSA_SUCCESS)
      {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)psa_ret);
      }
      else if (!s_init_psa_gcm_key())
      {
        ESP_LOGE(TAG, "Failed to import AES-GCM key into PSA — decryption will fail");
      }
      else
      {
        ESP_LOGI(TAG, "PSA AES-GCM key imported (id=%u)", (unsigned)s_aes_gcm_key_id);
      }

      // Schedule a login challenge so frame counters and AES-GCM nonces are
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
          // AES-GCM nonce map (cleared on hub reboot) unpopulated.
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
          uint32_t stagger_ms = static_cast<uint32_t>(this->short_address_) * LOGIN_STAGGER_MS;
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

    void LORAListener::save_state_(bool save)
    {
      if (save)
      {
        LORAClientRestoreState restore{};
        memset(&restore, 0, sizeof(restore));
        restore.version          = kRestoreStateVersion;
        restore.rx_message_id    = this->frame_counter_.rx_message_id;
        restore.tx_message_id    = this->frame_counter_.tx_message_id;
        restore.logged_in        = this->registered_;
        restore.last_sleep_epoch = this->last_sleep_epoch_;

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

    bool LORAListener::is_node_awake_() const
    {
      if (this->last_sleep_epoch_ == 0)
        return true; // Never slept or unknown — assume awake.
      if (this->time == nullptr || !this->time->now().is_valid())
        return true; // No valid clock — assume awake (best-effort).
      auto now     = static_cast<uint32_t>(this->time->now().timestamp);
      auto wake_at = this->last_sleep_epoch_ + static_cast<uint32_t>(this->sleep_duration_);
      return now >= wake_at;
    }

    uint32_t LORAListener::ms_until_node_awake_() const
    {
      if (this->is_node_awake_())
        return 0;
      auto now     = static_cast<uint32_t>(this->time->now().timestamp);
      auto wake_at = this->last_sleep_epoch_ + static_cast<uint32_t>(this->sleep_duration_);
      // Convert remaining seconds to ms, capped at uint32_t max (~49 days).
      return static_cast<uint32_t>(
          std::min<uint64_t>(static_cast<uint64_t>(wake_at - now) * 1000ULL,
                             static_cast<uint64_t>(0xFFFFFFFFu)));
    }

    void LORAListener::schedule_startup_login_()
    {
      this->cancel_timeout("login_startup");
      this->cancel_timeout("login_retry");

      // Per-node stagger: address × LOGIN_STAGGER_MS.
      // Prevents simultaneous login transmissions when all nodes happen to be
      // awake at hub reboot.  3 s gives enough headroom for a full challenge +
      // response exchange even at SF12 (~2.5 s airtime per packet).
      static constexpr uint32_t LOGIN_STAGGER_MS  = 3000;
      static constexpr uint32_t NODE_BOOT_MARGIN_MS = 5000;
      uint32_t stagger_ms = static_cast<uint32_t>(this->short_address_) * LOGIN_STAGGER_MS;

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
      // that the AES-GCM base-nonce map is repopulated (it is cleared on hub
      // reboot as a file-scope static and is never persisted to flash).
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

      this->send_login();   // resets frame counters to 0 + sends LoginMsg + BaseNonceExchange

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
      this->set_timeout("login_retry", static_cast<uint32_t>(delay), [this]()
      {
        if (this->login_acked_)
          return; // acked already; one-shot, nothing to re-arm
        if (++this->login_retry_count_ >= kMaxLoginRetries)
        {
          ESP_LOGE(TAG, "[%s] Login not acknowledged after %u retries — giving up",
                   this->get_name().c_str(), static_cast<unsigned>(kMaxLoginRetries));
          return;
        }
        ESP_LOGW(TAG, "[%s] Login not acknowledged — retry %u/%u",
                 this->get_name().c_str(),
                 static_cast<unsigned>(this->login_retry_count_),
                 static_cast<unsigned>(kMaxLoginRetries));
        this->send_login();
        this->schedule_login_retry_(); // re-arm the next one-shot
      });
    }

    // void LORAListener::send_remote_address()
    // {
    //   ESP_LOGI("LORAListener", "Sending remote address for device %s", this->address_str());
    //   // Implementation to send remote address

    //   BlndOperationMessage op_message BLND_OPERATION_MESSAGE__INIT;
    //   op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
    //   op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
    //   op_message.senderaddress = 0x12345678; // TODO: Use unique address
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
    //   op_message.senderaddress = 0x12345678; // TODO: Use unique address
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

    void LORAListener::set_response(uint8_t *data, size_t len)
    {
      LoraClientResponseMessage *rcv_message;

      rcv_message = lora_client_response_message__unpack(NULL, len, data);

      if (rcv_message == NULL)
      {
        ESP_LOGE(TAG, "Could not read protobuf");
        return;
      }

      // --- Encryption support helpers ---
      // s_base_nonce_map is file-scope so send_base_nonce_exchange() can also
      // access it.  The AES-GCM frame counter is NOT a separate counter — it is
      // exactly the LoraHeader.msgid value cast to uint64_t.  One unified counter
      // serves both replay protection and nonce derivation.

      auto u32_to_be = [](uint32_t value, uint8_t *bytes) {
        for (int i = 3; i >= 0; --i)
        {
          bytes[i] = static_cast<uint8_t>(value & 0xFF);
          value >>= 8;
        }
      };

      auto build_header_aad = [&](const LoraHeader *header, uint8_t *aad_out, size_t *aad_len) -> bool {
        if (!header || !aad_out || !aad_len)
          return false;
        u32_to_be(header->destaddress, aad_out);
        u32_to_be(header->destsubnet, aad_out + 4);
        u32_to_be(header->senderaddress, aad_out + 8);
        u32_to_be(header->msgid, aad_out + 12);
        // encrypted header field removed — AAD is the 16-byte 4-field header.
        *aad_len = 16;
        return true;
      };

      auto derive_gcm_nonce = [&](uint32_t peer_address, uint64_t frame_counter, uint8_t nonce_out[12]) -> bool {
        auto it = s_base_nonce_map.find(peer_address);
        if (it == s_base_nonce_map.end())
          return false;
        uint32_t base_nonce = it->second;
        u32_to_be(base_nonce, nonce_out);
        // append 8-byte frame counter BE
        for (int i = 7; i >= 0; --i)
        {
          nonce_out[4 + i] = static_cast<uint8_t>(frame_counter & 0xFF);
          frame_counter >>= 8;
        }
        return true;
      };

      // PSA AES-GCM decrypt — mirrors decrypt_payload_gcm() in BlindsESP CmdDispatcher.cpp.
      // The key parameter is accepted for API symmetry but is not used (the key is
      // held in the pre-imported PSA slot s_aes_gcm_key_id).
      auto decrypt_payload_gcm = [&](const uint8_t * /*key*/, const uint8_t *nonce,
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *cipher, size_t cipher_len,
                                     const uint8_t *tag,   size_t tag_len,
                                     uint8_t *plain_out) -> bool {
        if (!nonce || !cipher || !tag || !plain_out)
          return false;
        if (s_aes_gcm_key_id == PSA_KEY_ID_NULL && !s_init_psa_gcm_key())
          return false;

        // PSA psa_aead_decrypt() expects ciphertext || tag concatenated.
        size_t   ct_len = cipher_len + tag_len;
        uint8_t *ct_buf = static_cast<uint8_t *>(malloc(ct_len));
        if (!ct_buf)
          return false;
        memcpy(ct_buf,              cipher, cipher_len);
        memcpy(ct_buf + cipher_len, tag,    tag_len);

        size_t       plain_len = 0;
        psa_status_t status    = psa_aead_decrypt(
            s_aes_gcm_key_id, LORA_GCM_ALG,
            nonce,  kAesGcmIvBytes,
            aad,    aad_len,
            ct_buf, ct_len,
            plain_out, cipher_len, &plain_len);
        free(ct_buf);

        if (status != PSA_SUCCESS)
          ESP_LOGE(TAG, "psa_aead_decrypt failed: %d", (int)status);
        return status == PSA_SUCCESS;
      };


      // Registering a new device is allowed in all cases — checked before any
      // address or msgId filter so even a node whose address has changed gets through.
      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
      {
        ClientRegister *reg = rcv_message->register_;
        ESP_LOGI(TAG, "Received register command for MAC address: %d", reg->mac_addr);
        ESP_LOGI(TAG, "This clients' MAC address: %d", this->address_uint64_);
        if (reg->mac_addr != this->address_uint64_)
        {
          ESP_LOGE(TAG, "%s, MAC address does not match, ignoring register command", this->get_name().c_str());
          lora_client_response_message__free_unpacked(rcv_message, NULL);
          return;
        }

        ESP_LOGI(TAG, "%s, Registered with LORA server", this->get_name().c_str());
        this->registered_ = true;

        // Only (re)push configuration when the node reports it is unprovisioned
        // (needs_config), or when this hub boot has not yet pushed to the node
        // (!config_synced_ → delivers config changes flashed into the hub once
        // per boot).  A provisioned node waking from deep sleep keeps its
        // persisted config, so both are false and we skip the two ~1.5 s config
        // bursts and go straight to login — saving awake radio time on battery.
        const bool needs_cfg    = reg->needs_config;
        const bool push_config  = needs_cfg || !this->config_synced_;

        // Free before dispatching — nodes re-parse the raw bytes independently.
        lora_client_response_message__free_unpacked(rcv_message, NULL);

        if (push_config)
        {
          ESP_LOGI(TAG, "[%s] Pushing config (needs_config=%d synced_this_boot=%d)",
                   this->get_name().c_str(), (int)needs_cfg, (int)this->config_synced_);
          this->send_remote_config();   // ClientConfig (address, subnet, name, sleepDuration)
          // Dispatch REGISTER to all nodes so each can send its own configuration
          // (e.g. LoraCoverComponent sends CoverConfig; sensor is a no-op).
          for (auto *node : this->nodes_)
            node->set_response(data, len);
          this->config_synced_ = true;
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
        this->pending_login_nonce_      = 0;     // node just (re-)registered → mint fresh
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
        return;
      }





      if (rcv_message->header) {
        ESP_LOGI(TAG, "Received message with ID %d from address %d", rcv_message->header->msgid, rcv_message->header->senderaddress);
        ESP_LOGI(TAG, "My message ID: %d", this->frame_counter_.rx_message_id);
        ESP_LOGI(TAG, "My address: %d",  this->short_address_);
      } else {
        ESP_LOGI(TAG, "Received message without header");
      }

      if (this->registered_ == false)
      {
        ESP_LOGE(TAG, "%s, Not registered yet, ignoring message", this->get_name().c_str());
        lora_client_response_message__free_unpacked(rcv_message, NULL);
        return;
      }

      if (!(rcv_message->header) || (rcv_message->header->senderaddress != this->short_address_))
      {
        ESP_LOGE(TAG, "%s, Address not for me", this->get_name().c_str());
        lora_client_response_message__free_unpacked(rcv_message, NULL);
        return;
      }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_LOGIN)
      {
        // We ignore the prand form the client and send him a new one, to avoid replay attacks. 
        //The client has to use the prand we send him in the login response for all further communication, so it will be forced to use the new prand.
        // LoginMsg *login = rcv_message->login;
        // ESP_LOGI(TAG, "%s, Received login command with prand: %d", this->get_name().c_str(), login->prand);


        // this->frame_counter_.tx_message_id = login->prand;
        // this->frame_counter_.rx_message_id = login->prand;

        this->send_login();
        lora_client_response_message__free_unpacked(rcv_message, NULL);
        return;
      }


      // The message ID is checked only after login
      if (rcv_message->proto_case != LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
      {
        // Accept only a forward jump within a bounded window — a huge msgid from a
        // corrupt/spurious frame would otherwise ratchet rx up and wedge the link
        // (dropping every legitimate lower id) until the next login resets it.
        static constexpr uint32_t kMsgIdWindow = 1024;
        if (rcv_message->header &&
            rcv_message->header->msgid > this->frame_counter_.rx_message_id &&
            rcv_message->header->msgid <= this->frame_counter_.rx_message_id + kMsgIdWindow)
        {
          this->setRxMessageId(rcv_message->header->msgid);
          // NOTE: login is NOT acknowledged here.  A plaintext status frame (e.g.
          // a position report during boot homing) does not prove the node holds
          // the session key.  Marking login acked on any msgid-bump and then
          // encrypting downlink left a node that missed the LoginMsg unable to
          // decrypt commands.  login_acked_/session_confirmed_ are set only after
          // we successfully DECRYPT a frame from the node (encrypted branch below).
        }
        else
        {
          ESP_LOGE(TAG, "%s, duplicate or old message ID: %d, ignoring. My MsgID: %d",this->get_name().c_str(), (rcv_message->header? rcv_message->header->msgid : 0), this->frame_counter_.rx_message_id);
          lora_client_response_message__free_unpacked(rcv_message, NULL);
          return;
        }
      }


      // Distriute to registered nodes
      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ENCRYPTED && rcv_message->encrypted)
      {
        EncryptedPayload *enc = rcv_message->encrypted;
        ESP_LOGI(TAG, "Received encrypted payload ciphertext=%d bytes", (int)enc->ciphertext.len);

        // Use the header's msgid as the unified frame counter.  Replay protection
        // was already enforced above (msgid > frame_counter_.rx_message_id).
        // The IV must equal base_nonce || (uint64_t)msgid; if it doesn't the
        // GCM tag will also fail, but checking up-front avoids the decrypt cost.
        uint32_t sender      = rcv_message->header ? rcv_message->header->senderaddress : 0;
        uint64_t frame_counter = static_cast<uint64_t>(
            rcv_message->header ? rcv_message->header->msgid : 0u);

        // If we don't have a base nonce for this peer (e.g. recovery after reboot),
        // re-provision one, send it, and discard this packet — the node will restart
        // its encryption with the new nonce on the next transmission.
        if (s_base_nonce_map.find(sender) == s_base_nonce_map.end())
        {
          ESP_LOGW(TAG, "No base nonce for peer %u — re-provisioning", sender);
          this->send_base_nonce_exchange();
          lora_client_response_message__free_unpacked(rcv_message, NULL);
          return;
        }

        // Lightweight format: the IV is NOT on the wire — derive it locally from
        // base_nonce(peer) || (uint64) msgid.  If the sender used a different
        // msgid/nonce the GCM tag verification below fails, so the old explicit
        // IV memcmp is redundant.
        uint8_t iv[12];
        if (!derive_gcm_nonce(sender, frame_counter, iv))
        {
          ESP_LOGE(TAG, "Failed to derive GCM nonce for peer %u", sender);
          lora_client_response_message__free_unpacked(rcv_message, NULL);
          return;
        }

        uint8_t aad[20];
        size_t aad_len = 0;
        if (!build_header_aad(rcv_message->header, aad, &aad_len))
        {
          ESP_LOGE(TAG, "Failed to build AAD for encrypted response");
          lora_client_response_message__free_unpacked(rcv_message, NULL);
          return;
        }

        size_t cipher_len = enc->ciphertext.len;
        uint8_t *plaintext = static_cast<uint8_t *>(malloc(cipher_len));
        if (!plaintext)
        {
          ESP_LOGE(TAG, "Memory allocation failed for plaintext");
          lora_client_response_message__free_unpacked(rcv_message, NULL);
          return;
        }

        if (!decrypt_payload_gcm(nullptr,        // key unused — PSA slot used instead
                                 iv,
                                 aad,
                                 aad_len,
                                 enc->ciphertext.data,
                                 cipher_len,
                                 enc->tag.data,
                                 enc->tag.len,
                                 plaintext))
        {
          ESP_LOGE(TAG, "AES-GCM decryption/authentication failed");
          free(plaintext);
          lora_client_response_message__free_unpacked(rcv_message, NULL);
          return;
        }

        // A successful decrypt proves the node holds the matching base nonce —
        // the encrypted session is confirmed both ways.  Only now do we treat
        // login as acknowledged and allow the hub to encrypt downlink commands.
        this->session_confirmed_ = true;
        if (!this->login_acked_)
        {
          this->login_acked_         = true;
          this->login_retry_count_   = 0;
          this->pending_login_nonce_ = 0;
          this->cancel_timeout("login_retry");
          ESP_LOGI(TAG, "[%s] Login acknowledged by node (encrypted session confirmed)",
                   this->get_name().c_str());
        }

        // The inner is now payload-only (no header).  Unpack it, resolve the F-4
        // ack/position state machine, then RE-ATTACH the plaintext outer header
        // and re-pack before forwarding to nodes (loracover requires a header for
        // addressing — the header is no longer carried inside the ciphertext).
        LoraClientResponseMessage *inner =
            lora_client_response_message__unpack(NULL, cipher_len, plaintext);
        free(plaintext);
        if (inner)
        {
          if (inner->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK && inner->ack)
            this->handle_command_ack_(inner->ack->ack_msg_id);
          else if (inner->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION &&
                   this->op_awaiting_ack_)
            this->handle_command_ack_(this->op_last_msgid_); // position confirms delivery

          inner->header = rcv_message->header; // borrow outer header for forwarding
          size_t   fwd_len = lora_client_response_message__get_packed_size(inner);
          uint8_t *fwd     = static_cast<uint8_t *>(malloc(fwd_len));
          if (fwd)
          {
            lora_client_response_message__pack(inner, fwd);
            for (size_t i = 0; i < this->nodes_.size(); i++)
              this->nodes_[i]->set_response(fwd, fwd_len);
            free(fwd);
          }
          inner->header = nullptr; // detach borrowed header before free (rcv_message owns it)
          lora_client_response_message__free_unpacked(inner, NULL);
        }
        lora_client_response_message__free_unpacked(rcv_message, NULL);
        return;
      }

      // F-4: inspect plaintext responses for a CommandAck / position update.
      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK && rcv_message->ack)
        this->handle_command_ack_(rcv_message->ack->ack_msg_id);
      else if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION &&
               this->op_awaiting_ack_)
        this->handle_command_ack_(this->op_last_msgid_);

      for (int i = 0; i < this->nodes_.size(); i++)
      {
        this->nodes_[i]->set_response(data, len);
      }
      lora_client_response_message__free_unpacked(rcv_message, NULL);
    }

    // ---------------------------------------------------------------------------
    // F-4: Command ACK / retransmit state machine
    // ---------------------------------------------------------------------------
    uint32_t LORAListener::tx_cover_operation_()
    {
      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress   = this->short_address_;
      header.destsubnet    = this->subnet_address_;
      header.senderaddress = kHubAddress;
      header.msgid         = this->incrTxMessageId();
      op_message.header    = &header;
      op_message.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION;

      LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
      if (this->op_covop_case_ == LORA_COVER_OPERATION__COVOP_POSITION)
      {
        covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
        covop.position   = this->op_position_;
      }
      else
      {
        covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
        covop.operation  = static_cast<CovOperation>(this->op_operation_);
      }
      op_message.operation = &covop;

      // Encrypt the command in-session (AES-GCM) so the downlink cover op is
      // authenticated; falls back to plaintext only before a session exists.
      uint8_t *buf = nullptr;
      size_t   len = 0;
      if (s_pack_operation_message(&op_message, this->session_confirmed_, &buf, &len))
      {
        this->parent_->send(buf, len);
        free(buf);
      }
      else
      {
        ESP_LOGE(TAG, "[%s] Failed to pack cover operation", this->get_name().c_str());
      }
      return header.msgid;
    }

    void LORAListener::send_cover_operation(uint32_t covop_case, int32_t operation, float position)
    {
      this->op_covop_case_ = covop_case;
      this->op_operation_  = operation;
      this->op_position_   = position;

      uint32_t msgid = this->tx_cover_operation_();
      this->op_first_msgid_  = msgid;
      this->op_last_msgid_   = msgid;
      this->op_retry_count_  = 0;
      this->op_awaiting_ack_ = true;
      this->set_command_failed_(false);

      ESP_LOGI(TAG, "[%s] Cover op sent (msgid=%u) — awaiting ack", this->get_name().c_str(),
               (unsigned)msgid);

      this->cancel_timeout("op_retry");
      this->schedule_op_retry_();
    }

    void LORAListener::schedule_op_retry_()
    {
      // One-shot timeout: fires exactly kOpRetryIntervalMs from now (no random
      // interval phase offset), then re-arms itself.  This guarantees the first
      // retransmit waits the full interval, leaving the node's deferred ACK a
      // quiet window to arrive first.
      this->set_timeout("op_retry", kOpRetryIntervalMs, [this]() {
        if (!this->op_awaiting_ack_)
          return; // acked already; one-shot, nothing to re-arm
        if (++this->op_retry_count_ > kOpMaxRetries)
        {
          ESP_LOGE(TAG, "[%s] Cover op not acknowledged after %u retries — marking failed",
                   this->get_name().c_str(), (unsigned)kOpMaxRetries);
          this->op_awaiting_ack_ = false;
          this->set_command_failed_(true);
          // Half-open-session recovery: if the command failed while the encrypted
          // session is NOT confirmed, the node may have re-keyed (it has a session)
          // while the hub still thinks it has none — so the hub sends plaintext that
          // the node's Part B rejects.  Force an immediate fresh login (which resets
          // the retry backoff to fast) so the node re-sends its login-ack and the
          // session re-confirms in seconds instead of waiting out the backoff.
          if (!this->session_confirmed_)
          {
            ESP_LOGW(TAG, "[%s] Command failed with unconfirmed session — forcing re-login",
                     this->get_name().c_str());
            this->do_login_and_arm_retry_();
          }
          return;
        }
        uint32_t msgid = this->tx_cover_operation_();
        this->op_last_msgid_ = msgid;
        ESP_LOGW(TAG, "[%s] Cover op retransmit %u/%u (msgid=%u)", this->get_name().c_str(),
                 (unsigned)this->op_retry_count_, (unsigned)kOpMaxRetries, (unsigned)msgid);
        this->schedule_op_retry_(); // re-arm the next one-shot
      });
    }

    void LORAListener::handle_command_ack_(uint32_t ack_msg_id)
    {
      if (!this->op_awaiting_ack_)
        return;
      // Accept an ack for any msgid in the current command's transmit range
      // (first .. last) since each retransmit uses a fresh, higher msgid.
      if (ack_msg_id < this->op_first_msgid_ || ack_msg_id > this->op_last_msgid_)
        return;
      this->op_awaiting_ack_ = false;
      this->cancel_timeout("op_retry");
      this->set_command_failed_(false);
      ESP_LOGI(TAG, "[%s] Cover op acknowledged (ack_msg_id=%u)", this->get_name().c_str(),
               (unsigned)ack_msg_id);
    }

    void LORAListener::set_command_failed_(bool failed)
    {
      this->command_failed_ = failed;
      if (this->command_failed_bsensor_ != nullptr)
        this->command_failed_bsensor_->publish_state(failed);
    }

    void LORAListener::enterSleep()
    {
      ESP_LOGI("LORAListener", "Device %s entering sleep mode", this->address_str());

      // Persist the sleep timestamp before incrTxMessageId() triggers save_state_(),
      // so last_sleep_epoch_ is included in the same flash write.
      if (this->time != nullptr && this->time->now().is_valid())
        this->last_sleep_epoch_ = static_cast<uint32_t>(this->time->now().timestamp);
      this->login_acked_         = false;
      this->login_retry_count_   = 0;
      this->pending_login_nonce_ = 0;   // sleep ends the session; mint fresh on wake
      this->cancel_timeout("login_startup");
      this->cancel_timeout("login_retry");

      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress = this->short_address_;
      header.destsubnet = this->subnet_address_;
      header.senderaddress = kHubAddress; // TODO: Use unique address
      header.msgid = this->incrTxMessageId(); // Incrementing message ID
      op_message.header = &header;

      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
      op_message.sysop = CLIENT_OPERATION__CMD_SLEEP;

      // Encrypt the sysop in-session so SLEEP cannot be forged/injected.
      uint8_t *txBuf = nullptr;
      size_t   len   = 0;
      if (s_pack_operation_message(&op_message, this->session_confirmed_, &txBuf, &len))
      {
        this->parent_->send(txBuf, len);
        free(txBuf);
      }
      else
      {
        ESP_LOGE(TAG, "[%s] Failed to pack sleep operation", this->get_name().c_str());
      }

      // Schedule a login challenge slightly after the node is expected to wake up.
      // NODE_BOOT_MARGIN_MS gives the node time to complete its boot sequence and
      // send REGISTER, which cancels this timer (REGISTER-path is the primary path).
      // This timer is only the fallback for lost REGISTER packets.
      // LOGIN_STAGGER_MS per address unit prevents simultaneous login transmissions
      // when multiple nodes wake from sleep at roughly the same time.
      static constexpr uint32_t NODE_BOOT_MARGIN_MS = 5000;
      static constexpr uint32_t LOGIN_STAGGER_MS    = 3000;
      uint32_t stagger_ms = static_cast<uint32_t>(this->short_address_) * LOGIN_STAGGER_MS;
      uint32_t wake_ms = static_cast<uint32_t>(
          std::min<uint64_t>(this->sleep_duration_ * 1000ULL,
                             static_cast<uint64_t>(0xFFFFFFFFu)));
      uint32_t total_ms = wake_ms + NODE_BOOT_MARGIN_MS + stagger_ms;
      ESP_LOGI(TAG, "[%s] Sleep sent — login fallback timer in %u s (%u s sleep + %u s boot margin + %u s stagger)",
               this->get_name().c_str(), total_ms / 1000,
               wake_ms / 1000, NODE_BOOT_MARGIN_MS / 1000, stagger_ms / 1000);
      this->set_timeout("login_startup", total_ms,
                        [this]() { this->do_login_and_arm_retry_(); });
    }

    void LORAListener::triggerOTA()
    {
      ESP_LOGI("LORAListener", "Device %s triggering OTA", this->address_str());
      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress = this->short_address_;
      header.destsubnet = this->subnet_address_;
      header.senderaddress = kHubAddress; // TODO: Use unique address
      header.msgid = this->incrTxMessageId(); // Incrementing message ID
      op_message.header = &header;

      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
      op_message.sysop = CLIENT_OPERATION__CMD_OTA;

      // Encrypt the sysop in-session so OTA cannot be forged/injected.
      uint8_t *txBuf = nullptr;
      size_t   len   = 0;
      if (s_pack_operation_message(&op_message, this->session_confirmed_, &txBuf, &len))
      {
        this->parent_->send(txBuf, len);
        free(txBuf);
      }
      else
      {
        ESP_LOGE(TAG, "[%s] Failed to pack OTA operation", this->get_name().c_str());
      }
    }


    uint32_t LORAListener::incrTxMessageId()
    {
      uint32_t val = ++this->frame_counter_.tx_message_id;
      // Persist the updated tx id so reboots don't reuse IDs
      this->save_state_(true);
      return val;
    }

    void LORAListener::setRxMessageId(uint32_t msg_id)
    {
      this->frame_counter_.rx_message_id = msg_id;
      // Persist updated rx id
      this->save_state_(true);
    }

    void LORAListener::send_base_nonce_exchange()
    {
      // Generate a fresh base nonce for this peer and send it so the hub can
      // start encrypting responses.  Also resets the frame-counter state so
      // both sides start from zero after every (re-)login.
      uint32_t base = esp_random();
      s_base_nonce_map[this->short_address_] = base;

      uint8_t bn[4];
      // Encode base nonce as 4-byte big-endian
      bn[0] = (base >> 24) & 0xFF;
      bn[1] = (base >> 16) & 0xFF;
      bn[2] = (base >>  8) & 0xFF;
      bn[3] = (base >>  0) & 0xFF;

      BaseNonceExchange exchange = BASE_NONCE_EXCHANGE__INIT;
      exchange.base_nonce.data = bn;
      exchange.base_nonce.len  = 4;

      LoraClientOperationMessage op_message = LORA_CLIENT_OPERATION_MESSAGE__INIT;
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress   = this->short_address_;
      header.destsubnet    = this->subnet_address_;
      header.senderaddress = kHubAddress; // hub/controller address placeholder
      header.msgid         = this->incrTxMessageId();
      op_message.header    = &header;
      op_message.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_BASENONCE;
      op_message.basenonce = &exchange;

      size_t len    = lora_client_operation_message__get_packed_size(&op_message);
      uint8_t *buf  = new uint8_t[len];
      lora_client_operation_message__pack(&op_message, buf);
      this->parent_->send(buf, len);
      delete[] buf;

      ESP_LOGI(TAG, "Sent BaseNonceExchange (base=0x%08x) to peer %u",
               (unsigned)base, (unsigned)this->short_address_);
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
      }

      this->frame_counter_.tx_message_id = 0;
      this->frame_counter_.rx_message_id = 0;
      // New login challenge: the encrypted session is not confirmed until the
      // node proves it by sending a frame we can decrypt.  Until then the hub
      // sends commands in plaintext (see s_pack_operation_message callers).
      this->session_confirmed_ = false;

      // Store the base-nonce on the hub side before sending so it is ready to
      // validate the first encrypted response the node sends back.
      s_base_nonce_map[this->short_address_] = base;

      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress   = this->short_address_;
      header.destsubnet    = this->subnet_address_;
      header.senderaddress = kHubAddress;
      header.msgid         = this->incrTxMessageId();
      op_message.header    = &header;
      op_message.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN;

      LoginMsg login = LOGIN_MSG__INIT;
      // nonce carries the base-nonce for AES-GCM IV derivation.
      // The node stores it as its base_nonce on receipt of CMD_LOGIN.
      login.nonce = base;
      op_message.login = &login;

      uint8_t *txBuf;
      unsigned txLen;
      txLen  = lora_client_operation_message__get_packed_size(&op_message);
      txBuf  = new uint8_t[txLen];
      lora_client_operation_message__pack(&op_message, txBuf);
      this->parent_->send(txBuf, txLen);
      delete[] txBuf;

      ESP_LOGI(TAG, "[%s] LoginMsg sent (base_nonce=0x%08x msgid=%u)",
               this->get_name().c_str(), (unsigned)base,
               (unsigned)this->frame_counter_.tx_message_id);
    }

    void LORAListener::send_remote_config()
    {
      ESP_LOGI(TAG, "Sending remote config for device %s", this->address_str());
      // Implementation to send remote address

      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address

      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress = this->short_address_;
      header.destsubnet = this->subnet_address_;
      header.senderaddress = kHubAddress; // TODO: Use unique address
      header.msgid = this->incrTxMessageId(); //++(this->frame_counter_.tx_message_id); // Incrementing message ID
      op_message.header = &header;
      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG;

      ClientConfig clientconfig = CLIENT_CONFIG__INIT;
      clientconfig.mac_addr = this->address_uint64_;
      clientconfig.addr = this->short_address_;
      clientconfig.subnt = this->subnet_address_;
      clientconfig.name.data = (uint8_t *)this->get_name().c_str();
      clientconfig.name.len = strlen(this->get_name().c_str());
      clientconfig.sleepduration = this->sleep_duration_;

      // TimeUpdate time_update = TIME_UPDATE__INIT;
      // time_update.closetime = this->close_duration_;
      // time_update.opentime = this->open_duration_;

      ESP_LOGI(TAG, "Setting device name to: %s", this->get_name().c_str());

      op_message.clientconfig = &clientconfig;

      uint8_t *txBuf;
      unsigned len;
      len = lora_client_operation_message__get_packed_size(&op_message);
      txBuf = new uint8_t[len];
      lora_client_operation_message__pack(&op_message, txBuf);

      // this->parent_->sendPacketOnce(txBuf, len);
      this->parent_->send(txBuf, len);
      delete[] txBuf;
    }

  } // namespace lora_tracker
} // namespace esphome
