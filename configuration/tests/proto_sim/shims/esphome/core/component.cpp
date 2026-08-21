// Storage for the test-side hooks declared in component.h.
#include "esphome/core/component.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace esphome::shim_hooks {

namespace {
proto_sim::SimClock* g_active_clock = nullptr;
std::map<std::string, std::vector<uint8_t>> g_nvs;
} // namespace

proto_sim::SimClock* active_clock() { return g_active_clock; }
void set_active_clock(proto_sim::SimClock* c) { g_active_clock = c; }

std::vector<uint8_t>* nvs_slot_for(const std::string& entity_name) {
    return &g_nvs[entity_name];
}

void reset_nvs() { g_nvs.clear(); }

} // namespace esphome::shim_hooks
