// Shim implementation: route the hub's TX into the test SimRadio and
// implement register_client per the real lora_tracker.cpp:430 (post-fix
// version — set_parent BEFORE push_back).
#include "esphome/components/lora_tracker/lora_tracker.h"

#include "esphome/components/lora_client/lora_client.h"
#include "sim/messages.h"
#include "sim/sim_radio.h"
#include "TimedGrid.h"

#include <cstring>
#include <psa/crypto.h>

namespace esphome::lora_tracker {

namespace shim_hooks {
namespace {
proto_sim::SimRadio* g_active_radio = nullptr;
} // namespace
proto_sim::SimRadio* active_radio() { return g_active_radio; }
void set_active_radio(proto_sim::SimRadio* r) { g_active_radio = r; }
} // namespace shim_hooks

// Mirrors the production anchor arithmetic verbatim; the tests assert against
// this, so it must not "simplify" the negative-delta branch away.
void LORATracker::startGrid() {
    if (grid_started_) return;
    grid_anchor_us_ = 0;   // deterministic in the sim
    grid_started_ = true;
    psa_crypto_init();   // idempotent; mirrors production's call in startGrid
    // Mint a fleet key, deterministically. Production draws it from esp_random
    // in startGrid() for the same reason it sets the anchor there — the key's
    // lifetime is the grid's — but a sim that could not name the key could not
    // assert that the node adopted THAT key rather than some key.
    for (size_t i = 0; i < sizeof(net_key_); ++i)
        net_key_[i] = (uint8_t) (0xA0 + i);
    net_key_id_ = 0x5EED0001u;
}

// AES-CMAC, exactly as production computes it. Not a stub: the seam test's
// whole point is that the hub's tag verifies on the real node, and a stub tag
// would assert only that two mirrors of the same mistake agree.
bool LORATracker::beaconMac(uint32_t tx_round, uint32_t tx_slot,
                            uint32_t pending_mask, bool pending_mask_valid,
                            uint8_t *out, size_t out_len) const {
    if (out == nullptr || out_len != framecrypto::kBeaconMacBytes) return false;
    if (!framecrypto::netKeyIsSet(net_key_, sizeof(net_key_), net_key_id_))
        return false;

    uint8_t input[framecrypto::kBeaconMacInputBytes];
    framecrypto::buildBeaconMacInput(net_key_id_, tx_round, tx_slot,
                                     pending_mask, pending_mask_valid, input);

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_CMAC);
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, framecrypto::kNetKeyBytes * 8);
    psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);

    psa_key_id_t kid = PSA_KEY_ID_NULL;
    if (psa_import_key(&attrs, net_key_, sizeof(net_key_), &kid) != PSA_SUCCESS)
        return false;
    uint8_t full[16];
    size_t  full_len = 0;
    const psa_status_t st = psa_mac_compute(kid, PSA_ALG_CMAC, input,
                                            sizeof(input), full, sizeof(full),
                                            &full_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS || full_len < framecrypto::kBeaconMacBytes)
        return false;
    memcpy(out, full, framecrypto::kBeaconMacBytes);
    return true;
}

uint32_t LORATracker::roundForSlotT0(uint8_t slot, int64_t t0_us) const {
    if (!grid_started_) return 0;
    const int64_t rel = t0_us - grid_anchor_us_
                      - (int64_t) (slot % timedgrid::kSlotCount)
                          * (int64_t) timedgrid::kSlotPitchUs;
    if (rel < 0) return 0;
    return (uint32_t) (rel / (int64_t) timedgrid::kRoundUs);
}

int64_t LORATracker::nextT0ForSlotUs(uint8_t slot, int64_t now_us) const {
    if (!grid_started_) return now_us;
    const int64_t pitch = (int64_t) timedgrid::kSlotPitchUs;
    const int64_t round = (int64_t) timedgrid::kRoundUs;
    const int64_t base  = grid_anchor_us_ + (int64_t)(slot % timedgrid::kSlotCount) * pitch;
    const int64_t delta = now_us - base;
    if (delta <= 0) return base;
    return base + ((delta + round - 1) / round) * round;
}

uint32_t LORATracker::msUntilNextT0(uint8_t slot) const {
    if (!grid_started_) return 0;
    const int64_t t0 = nextT0ForSlotUs(slot, sim_now_us);
    const int64_t d  = t0 - sim_now_us;
    return d <= 0 ? 0u : (uint32_t)((d + 999) / 1000);
}

// Section 4.5's slot-aware deferral, mirrored. A test sets busy_until_us to
// stand a burst in the way; production computes it from the burst it is about
// to run.
int64_t LORATracker::busyUntilUs() const { return busy_until_us; }

int64_t LORATracker::nextClearT0ForSlotUs(uint8_t slot, int64_t now_us) const {
    const int64_t floor_us = (busy_until_us > now_us) ? busy_until_us : now_us;
    return nextT0ForSlotUs(slot, floor_us);
}

uint32_t LORATracker::msUntilNextClearT0(uint8_t slot) const {
    if (!grid_started_) return 0;
    const int64_t t0 = nextClearT0ForSlotUs(slot, sim_now_us);
    const int64_t d  = t0 - sim_now_us;
    return d <= 0 ? 0u : (uint32_t)((d + 999) / 1000);
}

bool LORATracker::send(uint8_t* data, size_t len, const TxPolicy& policy) {
    // Record the per-frame policy BEFORE the radio check, so a test can assert
    // what was requested even when no radio is attached.
    last_copies = policy.copies;
    sent_copies.push_back(policy.copies);
    last_earliest_us = policy.earliest_us;
    sent_earliest_us.push_back(policy.earliest_us);
    last_priority = policy.priority;

    // A simulated drop happens AFTER the policy is recorded and BEFORE anything
    // reaches the air: production drops in send() too, having already computed
    // the policy its caller asked for.
    if (drop_next_sends > 0) {
        --drop_next_sends;
        return false;
    }

    auto* r = shim_hooks::active_radio();
    if (!r) return true;
    proto_sim::AirFrame f{proto_sim::AirFrame::Dir::HubToNode,
                          std::vector<uint8_t>(data, data + len)};

    const int n = expand_bursts
                      ? (policy.copies > 0 ? policy.copies : default_copies)
                      : 1;
    for (int i = 0; i < n; ++i)
        r->send(f);
    return true;
}

void LORATracker::register_client(LORAClient* client) {
    client->set_parent(this);
    client->app_id = ++app_id_;
    clients_.push_back(client);
}

void LORATracker::register_listener(LORAListener* listener) {
    listener->set_parent(this);
    listeners_.push_back(listener);
}

} // namespace esphome::lora_tracker
