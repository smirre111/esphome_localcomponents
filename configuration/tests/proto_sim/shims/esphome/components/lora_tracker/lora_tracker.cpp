// Shim implementation: route the hub's TX into the test SimRadio and
// implement register_client per the real lora_tracker.cpp:430 (post-fix
// version — set_parent BEFORE push_back).
#include "esphome/components/lora_tracker/lora_tracker.h"

#include "esphome/components/lora_client/lora_client.h"
#include "sim/messages.h"
#include "sim/sim_radio.h"

namespace esphome::lora_tracker {

namespace shim_hooks {
namespace {
proto_sim::SimRadio* g_active_radio = nullptr;
} // namespace
proto_sim::SimRadio* active_radio() { return g_active_radio; }
void set_active_radio(proto_sim::SimRadio* r) { g_active_radio = r; }
} // namespace shim_hooks

void LORATracker::send(uint8_t* data, size_t len) {
    auto* r = shim_hooks::active_radio();
    if (!r) return;
    proto_sim::AirFrame f{proto_sim::AirFrame::Dir::HubToNode,
                          std::vector<uint8_t>(data, data + len)};
    r->send(f);
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
