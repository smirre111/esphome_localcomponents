// mac_mgmt.cpp -- the hub's MAC management unit. See mac_mgmt.h. Moved out of
// lora_tracker.cpp in stage S5 (beacon build, fleet key, beacon CMAC, shadow
// gather), byte-identical on the air (tests/proto_sim/golden/).
#include "mac_mgmt.h"

#include "lora_tracker.h"
#include "esphome/components/lora_client/lora_client.h"   // MAC-0 -> listener calls (fan-out, shadow gather)
#include "esphome/core/log.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/lora_client/TimedGrid.h"
#include "esphome/components/lora_client/PendingData.h"
#include "esphome/components/lora_client/MacWire.h"

#include <esp_random.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cstring>

namespace macmgmt
{

using esphome::lora_tracker::LORAListener;
using esphome::lora_tracker::LORATracker;

// The tracker's tag, so every moved log line reads exactly as before.
static const char *const TAG = "lora_tracker";

void HubMacManagement::mintFleetKey()
{
    // PSA is initialised HERE, not left to whichever component happened to
    // set up first. LORAListener::setup() initialises PSA too (through
    // LinkSecurity::initAuthKey), and the beacon tag used to be able to free-ride on
    // that — but component setup order is not a contract, and the failure it
    // produces is psa_import_key: -137 (BAD_STATE) at beacon time, which
    // reads as a crypto fault rather than as an ordering one. The call is
    // idempotent, which is what makes stating the dependency free.
    const psa_status_t psa_ret = psa_crypto_init();
    if (psa_ret != PSA_SUCCESS)
        ESP_LOGE(TAG, "psa_crypto_init failed: %d — beacons cannot be signed", (int) psa_ret);

    // Section 4.4's fleet key, minted WITH the anchor.
    //
    // Not derived from a compile-time secret, which was the obvious cheaper
    // option and is worth naming as rejected: a key every node can compute is
    // a key every node can forge under, and the whole point of authenticating
    // the beacon is that only the hub may move an anchor or clear a bit.
    //
    // Re-minted on every restart, and that costs nothing — a restart already
    // invalidates every node's anchor and forces GridSync to be re-published,
    // so the key's lifetime is exactly the grid's and there is nothing to
    // persist. The id is random for the same reason: a counter would need NVS
    // on a hub that has just lost the state a counter exists to survive.
    for (size_t i = 0; i < sizeof(this->net_key_); i += 4)
    {
        const uint32_t r = esp_random();
        std::memcpy(this->net_key_ + i, &r,
                    std::min(sizeof(uint32_t), sizeof(this->net_key_) - i));
    }
    // Zero means "no key" to every reader of this field, so a one-in-four-
    // billion draw must not be allowed to mean it.
    do { this->net_key_id_ = esp_random(); } while (this->net_key_id_ == 0);
    ESP_LOGI(TAG, "Fleet key minted for the grid (id %08x)", (unsigned) this->net_key_id_);
}

void HubMacManagement::addShadowSource(LORAListener *listener)
{
    this->sources_.push_back(listener);
}

// SHADOW pending mask: gather what each node's bit WOULD depend on, before the
// beacon enters the transmit queue. The node states are read on the loop task,
// which is where they are written.
void HubMacManagement::gatherShadow_(int64_t now_us, BeaconFrame *out) const
{
    out->shadow_in.reserve(this->sources_.size());
    out->shadow_names.reserve(this->sources_.size());
    for (LORAListener *l : this->sources_)
    {
        if (l == nullptr)
            continue;
        out->shadow_in.push_back(l->pending_shadow_inputs(now_us));
        out->shadow_names.push_back(l->get_name());
    }
}

// Builds and packs the signed beacon for `round`.
BeaconBuild HubMacManagement::buildBeacon(uint32_t round, int64_t now_us, size_t max_len,
                                          BeaconFrame *out)
{
    this->gatherShadow_(now_us, out);

    // A broadcast belongs to no per-node sequence (FCNT 0, packBeaconV1), and
    // the nodes exempt it from their replay filter for exactly that reason:
    // running it through one would ratchet every node's rx counter onto the
    // beacon's. Its replay protection is txRound -- a replayed beacon predicts
    // a mark rounds in the past, which no node will accept.
    GridBeacon gb = GRID_BEACON__INIT;
    gb.txround = round;
    gb.txslot  = timedgrid::kBeaconSlotIndex;
    // ALL LISTENING, deliberately, and still not a placeholder.
    //
    // A clear bit tells a node to stop listening for up to a beacon interval.
    // Two independent things had to be true before this hub could clear one.
    // ONE OF THEM IS NOW TRUE and the other is not:
    //
    //   * The beacon was unauthenticated, so the nodes refused a mask from it
    //     outright. That is fixed below: it carries an AES-CMAC under the
    //     fleet key, and a node holding that key adopts the mask.
    //   * A cleared bit is still a promise this hub cannot keep for an
    //     INTERACTIVE node: Home Assistant can produce a command at any
    //     instant, and the node would not be listening for up to 5.8 minutes.
    //     Tier 3 suits automatic-mode nodes, which are already unreachable
    //     between check-ins by design (section 5.5).
    //
    // So authenticating the beacon does NOT by itself start clearing bits.
    // What it buys today is the other thing an unauthenticated beacon could
    // do: nudge the anchor. The guard band bounded ONE nudge, not a sequence
    // of them, so anything in radio range could walk a node off the grid
    // 14 ms at a time. Only the hub can now.
    gb.pendingmask      = pending::allListening();
    gb.pendingmaskvalid = true;

    // Signed with the frame's FLEET tag over every raw byte (spec 4.4): the
    // burst index and every header field are covered as bytes. No timestamp: a
    // replayed beacon declares its own round, so its predicted mark is rounds in
    // the past and the node's reanchorIsSane already refuses it. No beacon
    // rather than an unsigned one (packBeaconV1 refuses without a key).
    return this->packBeaconV1(&gb, max_len, &out->bytes);
}

bool HubMacManagement::fleetTagV1(const uint8_t *bytes, size_t n, uint8_t tag[8]) const
{
    if (!framecrypto::netKeyIsSet(this->net_key_, sizeof(this->net_key_), this->net_key_id_))
        return false;
    if (bytes == nullptr || tag == nullptr || n > macwire::kMaxFrameBytes)
        return false;
    uint8_t in[macwire::kMacInputPrefixBytes + macwire::kMaxFrameBytes];
    macwire::buildMacInputPrefix(in, /*downlink=*/true, this->net_key_id_);
    std::memcpy(in + macwire::kMacInputPrefixBytes, bytes, n);

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_CMAC);
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, framecrypto::kNetKeyBytes * 8);
    psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);
    psa_key_id_t kid = PSA_KEY_ID_NULL;
    if (psa_import_key(&attrs, this->net_key_, sizeof(this->net_key_), &kid) != PSA_SUCCESS)
        return false;
    // Full width, then truncated: PSA refuses a short output buffer.
    uint8_t full[16];
    size_t  full_len = 0;
    const psa_status_t st = psa_mac_compute(kid, PSA_ALG_CMAC, in, macwire::kMacInputPrefixBytes + n,
                                            full, sizeof(full), &full_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS || full_len < 8)
        return false;
    std::memcpy(tag, full, 8);
    return true;
}

BeaconBuild HubMacManagement::packBeaconV1(::GridBeacon *gb, size_t max_len,
                                           std::vector<uint8_t> *out) const
{
    gb->netkeyid = this->net_key_id_;
    ::HubMacPdu pdu = HUB_MAC_PDU__INIT;
    pdu.pdu_case    = HUB_MAC_PDU__PDU_GRIDBEACON;
    pdu.gridbeacon  = gb;

    macwire::WireHdr h;
    h.flags     = macwire::withAuth(0, macwire::Auth::Fleet);
    h.dst       = macwire::kBroadcast;
    h.src       = (uint8_t) esphome::lora_tracker::kHubAddress;
    h.subnet    = macwire::kAnySubnet;
    h.fcnt      = 0;   // a broadcast: exempt from every replay window (spec 2.4)
    h.port      = macwire::kPortMac0Mgmt;
    h.burst_idx = 0;
    h.burst_cnt = 1;
    const size_t hlen = macwire::headerLen(h);
    const size_t blen = hub_mac_pdu__get_packed_size(&pdu);
    const size_t len  = hlen + blen + macwire::kTagBytes;
    if (len > max_len || len > macwire::kMaxFrameBytes)
    {
        ESP_LOGE(TAG, "beacon would not fit (%u B)", (unsigned) len);
        return BeaconBuild::TooBig;
    }
    out->assign(len, 0);
    macwire::encodeHeader(h, out->data(), len);
    hub_mac_pdu__pack(&pdu, out->data() + hlen);
    if (!this->fleetTagV1(out->data(), hlen + blen, out->data() + hlen + blen))
    {
        out->clear();
        ESP_LOGE(TAG, "beacon for round %u could not be signed — not sent", (unsigned) gb->txround);
        return BeaconBuild::Unsigned;
    }
    return BeaconBuild::Ok;
}

}  // namespace macmgmt
