#include "SessionManager.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "SessionMgr";

// F-5 persistence. Format is deliberately unchanged from when this lived in
// CmdDispatcher — same magic, version, namespace, key and field order — so a
// node upgraded to the refactored firmware restores its existing session.
static constexpr const char *kNvsNamespace = "loractr";
static constexpr const char *kNvsKey       = "peerstate";
static constexpr uint32_t    kMagic        = 0x4C525053; // 'LRPS'
static constexpr uint8_t     kVersion      = 1;

namespace
{
struct PersistState
{
    uint32_t magic;
    uint8_t  version;
    uint8_t  _pad[3];
    uint32_t hub_addr;
    uint32_t base_nonce;
    uint32_t tx_id;
    uint32_t rx_id;
};
}  // namespace

// ---------------------------------------------------------------------------
// Peer table
// ---------------------------------------------------------------------------

const SessionManager::PeerCounter *SessionManager::findPeer(uint32_t addr) const
{
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers_[i].valid && peers_[i].address == addr)
            return &peers_[i];
    return nullptr;
}

SessionManager::PeerCounter *SessionManager::findOrCreatePeer(uint32_t addr)
{
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers_[i].valid && peers_[i].address == addr)
            return &peers_[i];

    for (int i = 0; i < MAX_PEERS; ++i)
        if (!peers_[i].valid)
        {
            peers_[i] = PeerCounter{addr, 0, true};
            return &peers_[i];
        }

    // Table full — evict slot 0. Simplest policy that cannot fail, and with
    // MAX_PEERS = 4 against one hub it should never be reached.
    ESP_LOGW(TAG, "Peer table full, evicting slot 0 for addr %u", (unsigned) addr);
    peers_[0] = PeerCounter{addr, 0, true};
    return &peers_[0];
}

bool SessionManager::getBaseNonce(uint32_t peer, uint32_t &out) const
{
    const PeerCounter *p = findPeer(peer);
    if (p == nullptr)
        return false;

    // A zero nonce is the cleared/never-set sentinel, not a session. Reporting
    // it as usable let canResumeSession() claim a session that could not
    // decrypt anything.
    if (p->base_nonce == 0)
        return false;

    out = p->base_nonce;
    return true;
}

bool SessionManager::setBaseNonce(uint32_t peer, uint32_t nonce)
{
    PeerCounter *p = findOrCreatePeer(peer);
    if (p != nullptr)
        p->base_nonce = nonce;

    // The base nonce is the critical secret for resuming, so the caller should
    // persist immediately whenever it changes for the hub we track.
    return peer == persist_peer_;
}

void SessionManager::clearBaseNonce(uint32_t peer)
{
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers_[i].valid && peers_[i].address == peer)
        {
            peers_[i].base_nonce = 0;
            peers_[i].valid      = false;
        }
}

// ---------------------------------------------------------------------------
// Counters / replay
// ---------------------------------------------------------------------------

void SessionManager::resetCounters()
{
    tx_id_ = 0;
    rx_id_ = 0;
}

bool SessionManager::acceptRxId(uint32_t msgid)
{
    if (msgid > rx_id_ && msgid <= rx_id_ + kMsgIdWindow)
    {
        rx_id_ = msgid;
        maybePersist();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void SessionManager::load()
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "F-5: no persistent state (nvs_open: %s)", esp_err_to_name(err));
        return;
    }

    PersistState st{};
    size_t len = sizeof(st);
    err = nvs_get_blob(h, kNvsKey, &st, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(st) ||
        st.magic != kMagic || st.version != kVersion)
    {
        ESP_LOGW(TAG, "F-5: persistent state absent/invalid (err=%s len=%u) — "
                      "will rely on login",
                 esp_err_to_name(err), (unsigned) len);
        return;
    }

    // Restore rx as-is; restore tx with a reservation margin so any tx
    // increments that happened since the last save are never reused.
    rx_id_             = st.rx_id;
    tx_id_             = st.tx_id + kPersistTxMargin;
    last_persisted_tx_ = tx_id_;
    persist_peer_      = st.hub_addr;

    // Set the nonce directly rather than through setBaseNonce(), which would
    // report "persist me" and add another margin to the stored tx on every
    // boot (margin creep).
    PeerCounter *p = findOrCreatePeer(st.hub_addr);
    if (p != nullptr)
        p->base_nonce = st.base_nonce;

    valid_state_ = true;

    ESP_LOGI(TAG, "F-5: restored state hub=%u base_nonce=0x%08x tx=%u(+%u) rx=%u",
             (unsigned) st.hub_addr, (unsigned) st.base_nonce,
             (unsigned) st.tx_id, (unsigned) kPersistTxMargin, (unsigned) st.rx_id);
}

void SessionManager::save()
{
    uint32_t base_nonce = 0;
    if (!getBaseNonce(persist_peer_, base_nonce))
    {
        // Nothing worth saving: no session with the peer we track. Writing a
        // zero nonce here is what produced blobs that restored a session the
        // hub could never authenticate.
        return;
    }

    PersistState st{};
    st.magic      = kMagic;
    st.version    = kVersion;
    st.hub_addr   = persist_peer_;
    st.base_nonce = base_nonce;
    st.tx_id      = tx_id_;
    st.rx_id      = rx_id_;

    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "F-5: save nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(h, kNvsKey, &st, sizeof(st));
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "F-5: save failed: %s", esp_err_to_name(err));
        return;
    }

    last_persisted_tx_ = tx_id_;
    valid_state_       = true;
    ESP_LOGD(TAG, "F-5: NVS save — rx=%u tx=%u hub=%u",
             (unsigned) rx_id_, (unsigned) tx_id_, (unsigned) persist_peer_);
}

void SessionManager::maybePersist()
{
    if (tx_id_ >= last_persisted_tx_ + kPersistTxMargin)
        save();
}
