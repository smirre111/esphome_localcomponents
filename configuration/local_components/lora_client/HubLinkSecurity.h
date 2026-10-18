#pragma once
// HubLinkSecurity.h -- the hub's MAC-2 (security) sublayer.
//
// Stage S2 of the MAC separation refactor (docs/mac-separation-layering-plan.md,
// review docs/mac-review-2026-10-16.md sections 5.2/5.3/6). Everything the hub
// does with keys lives here, and nothing else in lora_client touches PSA:
//   * K_root (SHA-256 of the fleet key phrase, first 16 B) and K_auth (CMAC KDF),
//     the LOGIN/REGISTER MICs;
//   * the per-session K_enc/K_mac derivation (CMAC KDF over session id, node
//     nonce, hub and node address);
//   * Encrypt-then-CMAC: the downlink CTR seal, the per-copy downlink retag, the
//     uplink CMAC verify and CTR decrypt.
// SessionKeyStore is the SINGLE owner of every peer's session state (session
// id = base nonce, live K_enc/K_mac, generation, key epoch,
// installed session id). It replaces the listener members k_enc_key_id_,
// k_mac_key_id_, session_generation_, installed_session_id_ and the five static
// maps s_k_enc_map, s_k_mac_map, s_session_generation_map,
// s_seal_generation_map, s_base_nonce_map.
//
// The API speaks mac::MacHdr + bytes (MacSap.h), never protobuf: lora_client
// decodes the protobuf header into a MacHdr (refusing values that do not fit,
// see toMacHdr in lora_client.cpp) and packs/unpacks the payloads itself.
//
// HUB-ONLY (not vendored). The node's mirror is its own LinkSecurity.
//
// RULES THAT MOVED HERE INTACT (do not relax):
//   * keys_mutex_ (was s_session_keys_mutex) is taken only by the seal and the
//     retag (and by install/clear/forget and the accessors, which never call out
//     while holding it); the key id and the generation are READ AND USED inside
//     it -- a destroyed volatile key's slot id can be reused, so reading the id
//     and calling PSA after releasing could MAC under another session's key.
//   * Nothing in this unit logs (the callers log, outside any lock).
//   * The retag path builds its CMAC input on the stack: no heap, no virtual call
//     (H-1 measures it per copy).
//   * session_mutex_ (was s_base_nonce_map_mutex) and keys_mutex_ are never held
//     together, and PSA-internal locks are only ever taken inside keys_mutex_, so
//     there is no lock-order cycle.
//   * The generation is a monotonically increasing per-peer counter, never
//     reused (key ids are); a clear keeps it, forget (listener destroyed) drops it.
//     A frame is bound to the KEY EPOCH instead (FrameSeal), which also moves on
//     a clear, so a copy sealed before a fresh LOGIN challenge is refused too.

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

#include <psa/crypto.h>

#include "esphome/components/lora_client/FrameCrypto.h"
#include "esphome/components/lora_client/MacSap.h"

namespace mac2
{

static constexpr size_t kRootKeyBytes = 16;
static constexpr size_t kTagBytes     = framecrypto::kSessionCmacTagBytes;

// Which keys ONE sealed frame belongs to (S4, closes review F4). Travels with
// the frame (TxPolicy::seal_binding, the queued buffer) to the tracker, whose
// per-copy CopySealer call is refused once the peer's keys have moved on: the
// key epoch moves on every install AND every clear, so a frame sealed before a
// re-login is never tagged after it, however many frames are sealed since.
struct FrameSeal
{
    bool      sealed{false};   // false: a plaintext frame, no sealer
    mac::Addr peer{0};
    uint32_t  key_epoch{0};
    // The opaque 64-bit value MAC-0 stores with the frame and hands back to
    // LinkSecurity::copySealer as its ctx (a pointer to it).
    uint64_t binding() const
    {
        return sealed ? ((1ull << 63) | ((uint64_t) peer << 32) | key_epoch) : 0;
    }
    static FrameSeal fromBinding(uint64_t b)
    {
        FrameSeal f;
        f.sealed    = (b >> 63) != 0;
        f.peer      = static_cast<mac::Addr>((b >> 32) & 0xFF);
        f.key_epoch = static_cast<uint32_t>(b & 0xFFFFFFFFu);
        return f;
    }
};

// The two session keys of one peer (or one unverified candidate).
struct KeyPair
{
    psa_key_id_t enc{PSA_KEY_ID_NULL};
    psa_key_id_t mac{PSA_KEY_ID_NULL};
    bool complete() const { return enc != PSA_KEY_ID_NULL && mac != PSA_KEY_ID_NULL; }
    bool hasEnc() const { return enc != PSA_KEY_ID_NULL; }
};

// The single owner of every peer's MAC-2 session state. One per hub process
// (lora_client is MULTI_CONF; the tracker's retag up-call reaches it through
// the listener until S4).
class SessionKeyStore
{
public:
    // -- session id (the LOGIN's base nonce, input of the CTR block and KDF) --
    void setSessionId(mac::Addr peer, uint32_t session_id);
    // False when the peer has no entry; an entry holding 0 is found with 0.
    bool sessionId(mac::Addr peer, uint32_t *out) const;

    // -- live keys -----------------------------------------------------------
    // Install verified keys as the peer's live session: destroys the previous
    // ones, records session_id as installed, bumps the generation. Takes
    // ownership of `keys`.
    void install(mac::Addr peer, uint32_t session_id, const KeyPair &keys);
    // Destroy the peer's live keys (and so every frame sealed under them: the key
    // epoch moves). Keeps the session id and the generation counter.
    void clearKeys(mac::Addr peer);
    // Drop everything held for the peer (its listener is going away).
    void forget(mac::Addr peer);

    KeyPair  liveKeys(mac::Addr peer) const;
    uint32_t generation(mac::Addr peer) const;
    uint32_t installedSessionId(mac::Addr peer) const;
    // Burst copies refused because their frame was sealed under keys the peer
    // no longer holds (I3 / F4).
    uint32_t staleDrops(mac::Addr peer) const;

    // Test seams (characterization of the stale/failed retag rungs and of the
    // session-opening candidate decision).
    void setInstalledSessionIdForTest(mac::Addr peer, uint32_t session_id);
    void setMacKeyForTest(mac::Addr peer, psa_key_id_t id);

private:
    friend class LinkSecurity;
    struct Keys
    {
        KeyPair  live{};
        uint32_t generation{0};            // per peer, monotonic, never reused
        uint32_t installed_session_id{0};  // the session id `live` was derived for
        uint32_t key_epoch{0};             // moves on every install and clear (FrameSeal)
        uint32_t stale_drops{0};           // copies refused as stale
    };
    mutable std::mutex                session_mutex_;   // session_ids_
    std::map<mac::Addr, uint32_t>     session_ids_;
    mutable std::mutex                keys_mutex_;      // keys_
    std::map<mac::Addr, Keys>         keys_;
};

class LinkSecurity
{
public:
    // The hub's one instance (and its SessionKeyStore).
    static LinkSecurity &hub();
    SessionKeyStore &store() { return store_; }

    // -- K_root / K_auth (LOGIN and REGISTER) ----------------------------------
    static bool deriveRootKey(uint8_t out[kRootKeyBytes]);
    // Idempotent; derives K_auth from K_root on first use.
    bool initAuthKey();
    bool authKeyReady() const { return auth_key_ != PSA_KEY_ID_NULL; }
    // CMAC-8 under K_auth over a MIC input built by framecrypto (LOGIN).
    bool authMic(const uint8_t *mic_input, size_t len, uint8_t tag[kTagBytes]);
    // Verify a received MIC under K_auth (REGISTER). The tag must be exactly
    // kTagBytes long.
    bool verifyAuthMic(const uint8_t *mic_input, size_t len, const uint8_t *tag, size_t tag_len);

    // -- session keys ----------------------------------------------------------
    // Derive (never install) K_enc/K_mac for (session_id, node_nonce, hub, node).
    bool deriveCandidate(mac::Addr hub, mac::Addr node, uint32_t session_id,
                         uint32_t node_nonce, KeyPair *out);
    static void destroy(KeyPair &keys);

    // -- downlink --------------------------------------------------------------
    enum class Seal : uint8_t { Ok, MsgidSpent, NoSession, NoKey, Failed };
    // CTR-seal `pt` for hdr.dst under hdr.fcnt into `ct` (len bytes). On Ok,
    // *seal says which keys the frame belongs to (invariant I3, F4).
    Seal sealDownlink(const mac::MacHdr &hdr, const uint8_t *pt, size_t len, uint8_t *ct,
                      FrameSeal *seal);

    enum class Retag : uint8_t { Ok, MsgidSpent, NoSession, Stale, Failed };
    // The CMAC tag of one stamped burst copy of the frame `seal` describes: the
    // copy must be addressed to seal.peer and the peer must still hold the keys
    // the frame was sealed under (else Stale, counted). `tag` is written only on
    // Ok. Stack only, no logging.
    Retag retagCopy(const FrameSeal &seal, const mac::MacHdr &copy, const uint8_t *ct,
                    size_t ct_len, uint8_t tag[kTagBytes]);

    // The mac::CopySealer MAC-0 calls per copy: ctx points at the frame's
    // FrameSeal::binding(), stored with the queued frame.
    static bool copySealer(void *ctx, const mac::MacHdr &copy, const uint8_t *ct,
                           size_t ct_len, uint8_t tag[8]);

    // -- uplink ----------------------------------------------------------------
    enum class Verify : uint8_t { Ok, NoKeys, NoMem, BadTag };
    // CMAC check BEFORE any decryption. `keys` are the live keys or an
    // unverified candidate; only their MAC key is used here.
    Verify verifyUplink(const mac::MacHdr &hdr, uint32_t session_id, const KeyPair &keys,
                        const uint8_t *ct, size_t ct_len, const uint8_t *tag, size_t tag_len);
    // CTR-decrypt a verified uplink into `pt` (ct_len bytes).
    bool decryptUplink(const mac::MacHdr &hdr, uint32_t session_id, const KeyPair &keys,
                       const uint8_t *ct, size_t ct_len, uint8_t *pt);

    // For tests: CTR under the peer's CURRENT K_enc (proves hub and node landed on
    // bit-identical keys; opaque PSA ids cannot be compared).
    bool ctrEncryptForTest(mac::Addr peer, const uint8_t iv[16], const uint8_t *in, size_t len,
                           uint8_t *out);

private:
    Retag macUnderKeyLock_(const FrameSeal &seal, const uint8_t *in, size_t len,
                           uint8_t (&out)[kTagBytes]);
    SessionKeyStore store_;
    psa_key_id_t    auth_key_{PSA_KEY_ID_NULL};
};

}  // namespace mac2
