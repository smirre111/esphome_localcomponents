#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// SessionManager — everything that defines "do we have a working link to the
// hub": the per-peer base nonces, the message-id counters that provide replay
// protection and AEAD nonce derivation, and the NVS blob that carries all of it
// across deep sleep.
//
// Extracted from CmdDispatcher, which had grown to ~2900 lines and ten
// responsibilities. This was not a tidiness exercise: every hard-to-diagnose
// defect in this system lived in a seam BETWEEN two of those responsibilities,
// and three of them were in this one —
//
//   * persistHubAddr_ never tracked the real hub, so saves rewrote an ancient
//     broadcast-keyed blob and the resume beacon used a nonce the hub had
//     never held;
//   * get_base_nonce() reported a zero nonce as a usable session;
//   * the restored hub address was truncated through a uint8_t cast.
//
// All three were found on hardware, not by the tests, because the state was
// only reachable by constructing an entire CmdDispatcher with its motor, radio
// and mutexes. This class is constructible in one line, which is the point.
//
// The NVS format is deliberately UNCHANGED (same magic, version, namespace,
// key and field order), so a node that has been running the old firmware
// restores its existing session without noticing the refactor.
// ---------------------------------------------------------------------------

class SessionManager
{
public:
    // A peer entry exists as soon as counters are tracked for an address, so
    // `valid` means "we know this peer", NEVER "we have a session with it".
    // base_nonce == 0 is the cleared/never-set sentinel.
    struct PeerCounter
    {
        uint32_t address;
        uint32_t base_nonce;
        bool     valid;
    };

    static constexpr int MAX_PEERS = 4;

    // Accept only a forward jump within a bounded window. msgid increments by 1
    // per message (plus a reboot margin), so a huge jump is a corrupt or
    // spurious frame — ratcheting rx up to it would wedge the link until the
    // next login.
    static constexpr uint32_t kMsgIdWindow = 1024;

    // Reserve a gap on restore so tx increments that happened since the last
    // save are never reused. Also the write-throttle threshold: NVS is written
    // roughly once per this many messages.
    static constexpr uint32_t kPersistTxMargin = 64;

    // ---- base nonces -------------------------------------------------------

    // False when the peer is unknown OR its nonce is zero. A zero nonce is not
    // a session: encrypting with it produces frames the hub cannot authenticate.
    bool getBaseNonce(uint32_t peer, uint32_t &out) const;
    // Returns true when this peer is the one we persist for, i.e. the caller
    // should follow up with save(). Kept as a return value rather than saving
    // internally so the persistence policy stays visible at the call site.
    bool setBaseNonce(uint32_t peer, uint32_t nonce);
    void clearBaseNonce(uint32_t peer);

    // ---- message-id counters ----------------------------------------------

    uint32_t txId() const { return tx_id_; }
    uint32_t nextTxId() { return ++tx_id_; }
    uint32_t rxId() const { return rx_id_; }

    // A login resets both directions; the hub does the same in send_login().
    void resetCounters();

    // Replay filter. Advances rx and returns true when the id is a forward jump
    // inside the window; returns false (and changes nothing) otherwise.
    bool acceptRxId(uint32_t msgid);

    // ---- the peer we persist for ------------------------------------------
    //
    // This MUST be set to the hub we actually established a session with. It
    // defaulted to 0xFF and was only ever assigned from a loaded blob, so on a
    // node whose blob said 255 it stayed 255 forever while the live nonce sat
    // under the hub's real address.
    void     setPersistPeer(uint32_t addr) { persist_peer_ = addr; }
    uint32_t persistPeer() const { return persist_peer_; }

    // ---- persistence -------------------------------------------------------

    void load();
    void save();
    bool hasValidState() const { return valid_state_; }
    // Throttled save: writes only once tx has advanced a full margin, bounding
    // NVS wear.
    void maybePersist();

private:
    PeerCounter *findOrCreatePeer(uint32_t addr);
    const PeerCounter *findPeer(uint32_t addr) const;

    PeerCounter peers_[MAX_PEERS]{};
    uint32_t    tx_id_{0};
    uint32_t    rx_id_{0};
    uint32_t    last_persisted_tx_{0};
    uint32_t    persist_peer_{0xFF};
    bool        valid_state_{false};
};
