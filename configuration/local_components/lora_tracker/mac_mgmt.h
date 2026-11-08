#pragma once
// mac_mgmt.h -- the hub's MAC management unit (stage S5 of the MAC separation,
// docs/mac-separation-layering-plan.md, review sections 2.2 V2/V3 and 5.1).
//
// The management plane, out of the transmit engine: everything about the
// GridBeacon that is not "put these bytes on this mark" lives here --
//   * the fleet key and its id, minted with the grid anchor;
//   * the beacon's AUTH = FLEET tag (fleetTagV1, spec 4.4);
//   * the beacon itself, a MAC wire v1 frame (PORT 0, GridBeacon body).
// The tracker (MAC-0) asks buildBeacon() for one round and gets an OPAQUE frame;
// it never builds a beacon, holds the key or runs PSA for it.
//
// Hub-only. Wire output is byte-identical to before S5 (golden frames).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esphome/components/lora_client/FrameCrypto.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"

namespace esphome
{
  namespace lora_tracker
  {
    class LORAListener;
  }
}

namespace macmgmt
{

enum class BeaconBuild : uint8_t
{
    Ok,
    Unsigned,   // no key to sign with: no beacon rather than an unsigned one
    TooBig,     // the packed beacon would not fit the transmit buffer
};

// One beacon round, ready for MAC-0: the frame, opaque to the tracker.
struct BeaconFrame
{
    std::vector<uint8_t> bytes;
};

class HubMacManagement
{
public:
    // A fresh fleet key and id, with the grid anchor (startGrid). Initialises
    // PSA first (idempotent): component setup order is not a contract.
    void mintFleetKey();

    const uint8_t *netKey() const { return this->net_key_; }
    uint32_t       netKeyId() const { return this->net_key_id_; }


    // The signed beacon for `round` (at most max_len bytes).
    BeaconBuild buildBeacon(uint32_t round, size_t max_len, BeaconFrame *out);

    // MAC wire v1 (S8, docs/mac-wire-format.md 4.4): the AUTH = FLEET tag,
    // CMAC-8 under the fleet key over macwire::buildMacInputPrefix(downlink,
    // net_key_id) || bytes (= frame[0 .. L-8)). False without a key.
    bool fleetTagV1(const uint8_t *bytes, size_t n, uint8_t tag[8]) const;
    // The v1 beacon frame for `gb` (netkeyid filled here, no `mac` field):
    // HubMacPdu.gridbeacon behind a FLEET header, broadcast, FCNT 0, burst 0/1,
    // never stamped. buildBeacon's frame.
    BeaconBuild packBeaconV1(::GridBeacon *gb, size_t max_len, std::vector<uint8_t> *out) const;

    // Test seam: a hub that lost its key.
    uint8_t *netKeyForTest() { return this->net_key_; }
    // Test seam: a fixed key id (the S8 shared vectors fix both).
    void setNetKeyIdForTest(uint32_t id) { this->net_key_id_ = id; }

private:
    uint8_t  net_key_[framecrypto::kNetKeyBytes]{};
    uint32_t net_key_id_{0};
};

}  // namespace macmgmt
