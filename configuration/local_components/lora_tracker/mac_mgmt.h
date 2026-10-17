#pragma once
// mac_mgmt.h -- the hub's MAC management unit (stage S5 of the MAC separation,
// docs/mac-separation-layering-plan.md, review sections 2.2 V2/V3 and 5.1).
//
// The management plane, out of the transmit engine: everything about the
// GridBeacon that is not "put these bytes on this mark" lives here --
//   * the fleet key and its id, minted with the grid anchor;
//   * the beacon's AES-CMAC (beaconMac);
//   * the beacon itself, built as today's protobuf GridBeacon (the APP schema
//     until the wire stage, S8);
//   * the per-node inputs of the SHADOW pending mask, gathered from the
//     registered listeners.
// The tracker (MAC-0) asks buildBeacon() for one round and gets an OPAQUE frame
// plus the shadow inputs; it never builds a beacon, holds the key, runs PSA or
// iterates listeners for it.
//
// Hub-only. Wire output is byte-identical to before S5 (golden frames).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esphome/components/lora_client/FrameCrypto.h"
#include "esphome/components/lora_client/PendingShadow.h"

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

// One beacon round, ready for MAC-0: the frame (opaque to the tracker) and the
// shadow-mask inputs gathered at the same moment, before the beacon is queued
// (so the beacon itself does not count as "queued frames" for every node).
struct BeaconFrame
{
    std::vector<uint8_t>                   bytes;
    std::vector<pendingshadow::NodeInputs> shadow_in;
    std::vector<std::string>               shadow_names;
};

class HubMacManagement
{
public:
    // A fresh fleet key and id, with the grid anchor (startGrid). Initialises
    // PSA first (idempotent): component setup order is not a contract.
    void mintFleetKey();

    const uint8_t *netKey() const { return this->net_key_; }
    uint32_t       netKeyId() const { return this->net_key_id_; }

    // AES-CMAC (8 B) over the beacon's fields under the fleet key.
    bool beaconMac(uint32_t tx_round, uint32_t tx_slot, uint32_t pending_mask,
                   bool pending_mask_valid, uint32_t burst_index, uint8_t *out,
                   size_t out_len) const;

    // A listener whose state feeds the shadow pending mask.
    void   addShadowSource(esphome::lora_tracker::LORAListener *listener);
    size_t shadowSourceCount() const { return this->sources_.size(); }

    // The signed beacon for `round` (at most max_len bytes) and the shadow
    // inputs as of now_us.
    BeaconBuild buildBeacon(uint32_t round, int64_t now_us, size_t max_len, BeaconFrame *out);

    // Test seam: a hub that lost its key.
    uint8_t *netKeyForTest() { return this->net_key_; }

private:
    void gatherShadow_(int64_t now_us, BeaconFrame *out) const;

    uint8_t  net_key_[framecrypto::kNetKeyBytes]{};
    uint32_t net_key_id_{0};
    std::vector<esphome::lora_tracker::LORAListener *> sources_;
};

}  // namespace macmgmt
