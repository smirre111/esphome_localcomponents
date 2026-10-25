// HubLinkSecurity.cpp -- the hub's MAC-2 sublayer. See HubLinkSecurity.h for the
// rules this file must keep. Moved here from lora_client.cpp in stage S2
// (byte-identical wire output, pinned by tests/proto_sim/golden/).
#include "esphome/components/lora_client/HubLinkSecurity.h"
#include "esphome/components/lora_client/MacWire.h"

// LORA_FLEET_KEY comes from lora_tracker's `fleet_key` YAML option through
// cg.add_define (defines.h), or from the test build.
#include "esphome/core/defines.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace mac2
{

// Tier-1 key sourcing: the tracked source never contains the deployed value,
// only this named, obviously-a-placeholder dev fallback.
#ifdef LORA_FLEET_KEY
static constexpr const char *kLoRaRootKeyPhrase = LORA_FLEET_KEY;
#else
static constexpr const char *kLoRaRootKeyPhrase = "LoRaHome";
#endif

// Ciphertext bound for the stack-built retag input. A LoRa payload is at most
// 255 B, so a real ciphertext always fits.
static constexpr size_t kMaxCt = 256;

// ===========================================================================
// SessionKeyStore
// ===========================================================================

void SessionKeyStore::setSessionId(mac::Addr peer, uint32_t session_id)
{
    std::lock_guard<std::mutex> lock(this->session_mutex_);
    this->session_ids_[peer] = session_id;
}

bool SessionKeyStore::sessionId(mac::Addr peer, uint32_t *out) const
{
    std::lock_guard<std::mutex> lock(this->session_mutex_);
    auto it = this->session_ids_.find(peer);
    if (it == this->session_ids_.end())
        return false;
    *out = it->second;
    return true;
}

// Invariant I3 (c): install/clear race the tracker task's retag, which reads the
// key id and the generation; both sides hold keys_mutex_ over the read/write.
void SessionKeyStore::install(mac::Addr peer, uint32_t session_id, const KeyPair &keys)
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    Keys &k = this->keys_[peer];
    LinkSecurity::destroy(k.live);
    k.live                 = keys;
    k.installed_session_id = session_id;
    ++k.generation;
    ++k.key_epoch;
}

void SessionKeyStore::clearKeys(mac::Addr peer)
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    auto it = this->keys_.find(peer);
    if (it == this->keys_.end())
        return;
    LinkSecurity::destroy(it->second.live);
    it->second.installed_session_id = 0;
    ++it->second.key_epoch;
}

void SessionKeyStore::forget(mac::Addr peer)
{
    {
        std::lock_guard<std::mutex> lock(this->session_mutex_);
        this->session_ids_.erase(peer);
    }
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    auto it = this->keys_.find(peer);
    if (it == this->keys_.end())
        return;
    LinkSecurity::destroy(it->second.live);
    this->keys_.erase(it);
}

KeyPair SessionKeyStore::liveKeys(mac::Addr peer) const
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    auto it = this->keys_.find(peer);
    return it == this->keys_.end() ? KeyPair{} : it->second.live;
}

uint32_t SessionKeyStore::generation(mac::Addr peer) const
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    auto it = this->keys_.find(peer);
    return it == this->keys_.end() ? 0u : it->second.generation;
}

uint32_t SessionKeyStore::staleDrops(mac::Addr peer) const
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    auto it = this->keys_.find(peer);
    return it == this->keys_.end() ? 0u : it->second.stale_drops;
}

uint32_t SessionKeyStore::installedSessionId(mac::Addr peer) const
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    auto it = this->keys_.find(peer);
    return it == this->keys_.end() ? 0u : it->second.installed_session_id;
}

uint32_t SessionKeyStore::keyEpoch(mac::Addr peer) const
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    auto it = this->keys_.find(peer);
    return it == this->keys_.end() ? 0u : it->second.key_epoch;
}

void SessionKeyStore::setInstalledSessionIdForTest(mac::Addr peer, uint32_t session_id)
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    this->keys_[peer].installed_session_id = session_id;
}

void SessionKeyStore::setMacKeyForTest(mac::Addr peer, psa_key_id_t id)
{
    std::lock_guard<std::mutex> lock(this->keys_mutex_);
    this->keys_[peer].live.mac = id;
}

// ===========================================================================
// Key material
// ===========================================================================

LinkSecurity &LinkSecurity::hub()
{
    static LinkSecurity instance;
    return instance;
}

void LinkSecurity::destroy(KeyPair &keys)
{
    if (keys.enc != PSA_KEY_ID_NULL) psa_destroy_key(keys.enc);
    if (keys.mac != PSA_KEY_ID_NULL) psa_destroy_key(keys.mac);
    keys = KeyPair{};
}

// K_root = SHA-256(kLoRaRootKeyPhrase)[0:16]. Mirrors derive_root_key() on the
// node; changing one byte breaks every frame on the air, so its output is pinned
// by a test (HubRootKey in real_lora_client_test.cpp).
bool LinkSecurity::deriveRootKey(uint8_t key_out[kRootKeyBytes])
{
    uint8_t hash[32];
    size_t  hash_len = 0;
    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(kLoRaRootKeyPhrase),
        strlen(kLoRaRootKeyPhrase), hash, sizeof(hash), &hash_len);
    if (status != PSA_SUCCESS)
        return false;
    memcpy(key_out, hash, kRootKeyBytes);
    return true;
}

// Mirrors comm_utils.c's comm_utils_import_cmac_key()/comm_utils_import_ctr_key()
// on the node.
static bool s_import_cmac_key(const uint8_t *key_material, size_t key_bytes, psa_key_id_t *out_id)
{
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE |
                                        PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attrs, PSA_ALG_AT_LEAST_THIS_LENGTH_MAC(PSA_ALG_CMAC, 8));
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, (psa_key_bits_t) (key_bytes * 8));
    psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);
    *out_id = PSA_KEY_ID_NULL;
    return psa_import_key(&attrs, key_material, key_bytes, out_id) == PSA_SUCCESS;
}

static bool s_import_ctr_key(const uint8_t *key_material, size_t key_bytes, psa_key_id_t *out_id)
{
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_CTR);
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, (psa_key_bits_t) (key_bytes * 8));
    psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);
    *out_id = PSA_KEY_ID_NULL;
    return psa_import_key(&attrs, key_material, key_bytes, out_id) == PSA_SUCCESS;
}

// K_root imported as a CMAC key, for the KDFs. The material is wiped.
static bool s_import_root_key(psa_key_id_t *out_id)
{
    uint8_t root_material[kRootKeyBytes];
    if (!LinkSecurity::deriveRootKey(root_material))
        return false;
    const bool ok = s_import_cmac_key(root_material, sizeof(root_material), out_id);
    memset(root_material, 0, sizeof(root_material));
    return ok;
}

// One CMAC-KDF step: 16 B of key material = CMAC-16(K_root, input).
static bool s_kdf(psa_key_id_t root_key_id, const uint8_t (&input)[framecrypto::kKdfInputBytes],
                  uint8_t (&out)[kRootKeyBytes])
{
    size_t len = 0;
    return psa_mac_compute(root_key_id, PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, kRootKeyBytes),
                           input, sizeof(input), out, sizeof(out), &len) == PSA_SUCCESS &&
           len == kRootKeyBytes;
}

// K_auth = AES-CMAC(K_root, framecrypto::buildKAuthKdfInput()), once. Mirrors
// CmdDispatcher::init_k_auth_() on the node.
bool LinkSecurity::initAuthKey()
{
    if (this->auth_key_ != PSA_KEY_ID_NULL)
        return true;
    if (psa_crypto_init() != PSA_SUCCESS)
        return false;
    psa_key_id_t root_key_id = PSA_KEY_ID_NULL;
    if (!s_import_root_key(&root_key_id))
        return false;
    uint8_t kdf_input[framecrypto::kKdfInputBytes];
    framecrypto::buildKAuthKdfInput(kdf_input);
    uint8_t material[kRootKeyBytes];
    const bool derived = s_kdf(root_key_id, kdf_input, material);
    psa_destroy_key(root_key_id);
    if (!derived)
        return false;
    psa_key_id_t id = PSA_KEY_ID_NULL;
    const bool ok = s_import_cmac_key(material, sizeof(material), &id);
    memset(material, 0, sizeof(material));
    this->auth_key_ = ok ? id : PSA_KEY_ID_NULL;
    return ok;
}

// K_enc/K_mac from (K_root, session_id, node_nonce, hub/node addresses). Mirrors
// CmdDispatcher::deriveSessionKeys_() on the node.
bool LinkSecurity::deriveCandidate(mac::Addr hub, mac::Addr node, uint32_t session_id,
                                   uint32_t node_nonce, KeyPair *out)
{
    psa_key_id_t root_key_id = PSA_KEY_ID_NULL;
    if (!s_import_root_key(&root_key_id))
        return false;
    uint8_t enc_input[framecrypto::kKdfInputBytes];
    uint8_t mac_input[framecrypto::kKdfInputBytes];
    framecrypto::buildSessionKeyKdfInput(0x01, session_id, node_nonce, hub, node, enc_input);
    framecrypto::buildSessionKeyKdfInput(0x02, session_id, node_nonce, hub, node, mac_input);
    uint8_t k_enc[kRootKeyBytes];
    uint8_t k_mac[kRootKeyBytes];
    const bool enc_ok = s_kdf(root_key_id, enc_input, k_enc);
    const bool mac_ok = s_kdf(root_key_id, mac_input, k_mac);
    psa_destroy_key(root_key_id);
    KeyPair kp;
    const bool imported = enc_ok && mac_ok &&
                          s_import_ctr_key(k_enc, sizeof(k_enc), &kp.enc) &&
                          s_import_cmac_key(k_mac, sizeof(k_mac), &kp.mac);
    memset(k_enc, 0, sizeof(k_enc));
    memset(k_mac, 0, sizeof(k_mac));
    if (!imported)
    {
        destroy(kp);
        return false;
    }
    *out = kp;
    return true;
}

// ===========================================================================
// Encrypt-then-CMAC
// ===========================================================================

// One AES-CTR pass of in -> out (in_len bytes) under key_id. The caller holds
// keys_mutex_ whenever the key could be destroyed concurrently.
static bool s_ctr_pass(bool encrypt, psa_key_id_t key_id,
                       const uint8_t (&ctr_block)[framecrypto::kCtrBlockBytes],
                       const uint8_t *in, size_t in_len, uint8_t *out)
{
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    bool ok = (encrypt ? psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CTR)
                       : psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_CTR)) == PSA_SUCCESS;
    if (ok) ok = psa_cipher_set_iv(&op, ctr_block, sizeof(ctr_block)) == PSA_SUCCESS;
    size_t out_total = 0, part_len = 0;
    if (ok)
    {
        ok = psa_cipher_update(&op, in, in_len, out, in_len, &part_len) == PSA_SUCCESS;
        out_total += part_len;
    }
    if (ok)
    {
        size_t finish_len = 0;
        ok = psa_cipher_finish(&op, out + out_total, in_len - out_total, &finish_len) == PSA_SUCCESS;
        out_total += finish_len;
        ok = ok && (out_total == in_len);
    }
    else
    {
        psa_cipher_abort(&op);
    }
    return ok;
}

// The ONLY part of the retag that holds keys_mutex_: the peer's key epoch and
// K_mac are read AND USED inside it (I3, I3 (c), F4). One psa_mac_compute, no
// logging, no heap.
LinkSecurity::Retag LinkSecurity::macUnderKeyLock_(const FrameSeal &seal, const uint8_t *in,
                                                   size_t len, uint8_t (&out)[kTagBytes])
{
    std::lock_guard<std::mutex> lock(this->store_.keys_mutex_);
    auto s = this->store_.keys_.find(seal.peer);
    if (s == this->store_.keys_.end())
        return Retag::Stale;
    if (s->second.key_epoch != seal.key_epoch)
    {
        ++s->second.stale_drops;
        return Retag::Stale;
    }
    const psa_key_id_t mac_key = s->second.live.mac;
    if (mac_key == PSA_KEY_ID_NULL)
        return Retag::Failed;
    size_t out_len = 0;
    const bool ok = psa_mac_compute(mac_key, PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, kTagBytes), in,
                                    len, out, sizeof(out), &out_len) == PSA_SUCCESS &&
                    out_len == sizeof(out);
    return ok ? Retag::Ok : Retag::Failed;
}

// Runs on the ESPHome loop, the same task that installs/destroys keys, so the
// key cannot vanish mid-pass (as before S2).
bool LinkSecurity::decryptUplink(const mac::MacHdr &hdr, uint32_t session_id, const KeyPair &keys,
                                 const uint8_t *ct, size_t ct_len, uint8_t *pt)
{
    uint8_t ctr_block[framecrypto::kCtrBlockBytes];
    framecrypto::buildCtrInitialBlock(session_id, hdr.src, /*downlink=*/false, hdr.fcnt,
                                      /*block_idx=*/0, ctr_block);
    return s_ctr_pass(false, keys.enc, ctr_block, ct, ct_len, pt);
}

// ===========================================================================
// MAC wire v1 (S8): the tag covers the raw transmitted bytes
// ===========================================================================

// prefix || bytes on the stack (<= 8 + 255). False when it would not fit.
static bool s_v1_mac_input(bool downlink, uint32_t id, const uint8_t *bytes, size_t n,
                           uint8_t (&in)[macwire::kMacInputPrefixBytes + kMaxCt], size_t *len)
{
    if (n > kMaxCt || (n > 0 && bytes == nullptr))
        return false;
    macwire::buildMacInputPrefix(in, downlink, id);
    if (n)
        memcpy(in + macwire::kMacInputPrefixBytes, bytes, n);
    *len = macwire::kMacInputPrefixBytes + n;
    return true;
}

static bool s_cmac8(psa_key_id_t key, const uint8_t *in, size_t len, uint8_t (&tag)[kTagBytes])
{
    size_t out_len = 0;
    return key != PSA_KEY_ID_NULL &&
           psa_mac_compute(key, PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, kTagBytes), in, len, tag,
                           sizeof(tag), &out_len) == PSA_SUCCESS &&
           out_len == sizeof(tag);
}

// A whole frame's header, read back: dst and FCNT are what the CTR block and
// the epoch binding need.
static bool s_v1_frame_ok(const uint8_t *frame, size_t len, macwire::WireHdr *h)
{
    size_t off = 0, body = 0;
    return frame != nullptr && macwire::decode(frame, len, h, &off, &body);
}

// Pack time, not the per-copy path: the work buffers live on the heap because
// this also runs on the 3.5 KB esp_timer task (StackWatch.h). Allocated by the
// caller BEFORE keys_mutex_ is taken.
struct V1SealWork
{
    uint8_t ct[kMaxCt];
    uint8_t in[macwire::kMacInputPrefixBytes + kMaxCt];
};

// Encrypt the body in place and tag the whole frame under one peer's live keys.
// The caller holds keys_mutex_ (the ids are read AND used inside it).
static bool s_v1_seal_locked(const KeyPair &live, uint32_t session_id,
                             const uint8_t (&ctr_block)[framecrypto::kCtrBlockBytes],
                             uint8_t *frame, size_t hdr_len, size_t body_len, V1SealWork &w)
{
    if (body_len && !s_ctr_pass(true, live.enc, ctr_block, frame + hdr_len, body_len, w.ct))
        return false;
    if (body_len)
        memcpy(frame + hdr_len, w.ct, body_len);
    size_t  in_len = 0;
    uint8_t tag[kTagBytes];
    if (!s_v1_mac_input(true, session_id, frame, hdr_len + body_len, w.in, &in_len) ||
        !s_cmac8(live.mac, w.in, in_len, tag))
        return false;
    memcpy(frame + hdr_len + body_len, tag, sizeof(tag));
    return true;
}

LinkSecurity::Seal LinkSecurity::sealFrameV1(uint8_t *frame, size_t hdr_len, size_t body_len,
                                             FrameSeal *seal)
{
    macwire::WireHdr h;
    if (body_len > kMaxCt || !s_v1_frame_ok(frame, hdr_len + body_len + kTagBytes, &h))
        return Seal::Failed;
    if (h.fcnt >= macwire::kFcntLimit)
        return Seal::MsgidSpent;
    uint32_t session_id = 0;
    this->store_.sessionId(h.dst, &session_id);
    if (session_id == 0)
        return Seal::NoSession;
    uint8_t ctr_block[framecrypto::kCtrBlockBytes];
    framecrypto::buildCtrInitialBlock(session_id, h.dst, /*downlink=*/true, h.fcnt, 0, ctr_block);

    std::unique_ptr<V1SealWork> work(new (std::nothrow) V1SealWork);
    if (!work)
        return Seal::Failed;
    std::lock_guard<std::mutex> lock(this->store_.keys_mutex_);
    auto it = this->store_.keys_.find(h.dst);
    if (it == this->store_.keys_.end() || !it->second.live.complete())
        return Seal::NoKey;
    if (!s_v1_seal_locked(it->second.live, session_id, ctr_block, frame, hdr_len, body_len, *work))
        return Seal::Failed;
    seal->sealed    = true;
    seal->kauth     = false;
    seal->peer      = h.dst;
    seal->key_epoch = it->second.key_epoch;
    return Seal::Ok;
}

bool LinkSecurity::authTagV1_(bool downlink, const uint8_t *bytes, size_t n,
                              uint8_t (&tag)[kTagBytes])
{
    uint8_t in[macwire::kMacInputPrefixBytes + kMaxCt];
    size_t  in_len = 0;
    return this->initAuthKey() && s_v1_mac_input(downlink, 0, bytes, n, in, &in_len) &&
           s_cmac8(this->auth_key_, in, in_len, tag);
}

bool LinkSecurity::authFrameV1(bool downlink, uint8_t *frame, size_t len, FrameSeal *seal)
{
    macwire::WireHdr h;
    uint8_t tag[kTagBytes];
    if (!s_v1_frame_ok(frame, len, &h) || h.auth() != macwire::Auth::KAuth ||
        !this->authTagV1_(downlink, frame, len - kTagBytes, tag))
        return false;
    memcpy(frame + len - kTagBytes, tag, sizeof(tag));
    if (seal != nullptr)
    {
        *seal        = FrameSeal{};
        seal->sealed = true;
        seal->kauth  = true;
        seal->peer   = h.dst;
    }
    return true;
}

bool LinkSecurity::verifyAuthFrameV1(bool downlink, const uint8_t *frame, size_t len)
{
    macwire::WireHdr h;
    if (!s_v1_frame_ok(frame, len, &h) || h.auth() != macwire::Auth::KAuth)
        return false;
    uint8_t in[macwire::kMacInputPrefixBytes + kMaxCt];
    size_t  in_len = 0;
    return this->initAuthKey() && s_v1_mac_input(downlink, 0, frame, len - kTagBytes, in, &in_len) &&
           psa_mac_verify(this->auth_key_, PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, kTagBytes), in,
                          in_len, frame + len - kTagBytes, kTagBytes) == PSA_SUCCESS;
}

LinkSecurity::Verify LinkSecurity::verifyUplinkV1(uint32_t session_id, const KeyPair &keys,
                                                  const uint8_t *frame, size_t len)
{
    if (!keys.complete())
        return Verify::NoKeys;
    macwire::WireHdr h;
    if (!s_v1_frame_ok(frame, len, &h) || h.auth() != macwire::Auth::Session)
        return Verify::BadTag;
    uint8_t in[macwire::kMacInputPrefixBytes + kMaxCt];
    size_t  in_len = 0;
    if (!s_v1_mac_input(false, session_id, frame, len - kTagBytes, in, &in_len))
        return Verify::BadTag;
    return psa_mac_verify(keys.mac, PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, kTagBytes), in, in_len,
                          frame + len - kTagBytes, kTagBytes) == PSA_SUCCESS
               ? Verify::Ok
               : Verify::BadTag;
}

// The v1 per-copy path (H-1): the input is built on the stack before the lock,
// the lock covers one psa_mac_compute (macUnderKeyLock_), no heap, no logging.
LinkSecurity::Retag LinkSecurity::retagFrameV1(const FrameSeal &seal, const mac::MacHdr &copy,
                                               const uint8_t *bytes, size_t n,
                                               uint8_t tag[kTagBytes])
{
    if (copy.fcnt >= macwire::kFcntLimit)
        return Retag::MsgidSpent;
    uint32_t session_id = 0;
    if (!seal.sealed || !this->store_.sessionId(seal.peer, &session_id))
        return Retag::NoSession;
    uint8_t in[macwire::kMacInputPrefixBytes + kMaxCt];
    size_t  in_len = 0;
    if (!s_v1_mac_input(true, session_id, bytes, n, in, &in_len))
        return Retag::Failed;
    uint8_t   out[kTagBytes];
    FrameSeal bound = seal;
    if (copy.dst != seal.peer)
        bound.key_epoch = ~seal.key_epoch;   // a copy for another node: never this frame's
    const Retag verdict = this->macUnderKeyLock_(bound, in, in_len, out);
    if (verdict == Retag::Ok)
        memcpy(tag, out, sizeof(out));
    return verdict;
}

bool LinkSecurity::copySealerV1(void *ctx, const mac::MacHdr &copy, const uint8_t *bytes, size_t n,
                                uint8_t tag[8])
{
    if (ctx == nullptr)
        return false;
    const FrameSeal seal = FrameSeal::fromBinding(*static_cast<const uint64_t *>(ctx));
    if (!seal.sealed)
        return false;
    if (!seal.kauth)
        return hub().retagFrameV1(seal, copy, bytes, n, tag) == Retag::Ok;
    uint8_t out[kTagBytes];
    if (!hub().authTagV1_(/*downlink=*/true, bytes, n, out))
        return false;
    memcpy(tag, out, sizeof(out));
    return true;
}

bool LinkSecurity::ctrEncryptForTest(mac::Addr peer, const uint8_t iv[16], const uint8_t *in,
                                     size_t len, uint8_t *out)
{
    const KeyPair live = this->store_.liveKeys(peer);
    if (live.enc == PSA_KEY_ID_NULL || len == 0)
        return false;
    uint8_t block[framecrypto::kCtrBlockBytes];
    memcpy(block, iv, sizeof(block));
    return s_ctr_pass(true, live.enc, block, in, len, out);
}

}  // namespace mac2
