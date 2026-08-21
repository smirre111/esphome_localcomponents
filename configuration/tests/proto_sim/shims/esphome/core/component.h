// Host-side shim for ESPHome's Component + EntityBase. The scheduler
// methods route into a process-wide SimClock instance configured by the
// test fixture via real_test::set_active_clock().
#pragma once

#include "esphome/core/helpers.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace proto_sim { class SimClock; }

namespace esphome {

// Test-side hooks: scenarios attach a SimClock + a NVS slot store before
// constructing real LORAListener instances. Defined in
// shims/esphome/core/component.cpp.
namespace shim_hooks {
proto_sim::SimClock* active_clock();
void set_active_clock(proto_sim::SimClock* c);

// Per-entity NVS slot. The first make_entity_preference<T> call for an
// entity name allocates the slot; subsequent calls reuse it.
std::vector<uint8_t>* nvs_slot_for(const std::string& entity_name);
void reset_nvs();
} // namespace shim_hooks

class EntityBase {
public:
    EntityBase() = default;
    virtual ~EntityBase() = default;

    const std::string& get_name() const { return name_; }
    void               set_name(std::string n) { name_ = std::move(n); }
    // Production EntityBase also has set_object_id, set_icon, etc. — not
    // referenced by lora_client.

private:
    std::string name_;
};

class Component {
public:
    Component() = default;
    virtual ~Component() = default;

    virtual void setup() {}
    virtual void dump_config() {}
    virtual void loop() {}

    // Scheduler — routes into the active SimClock with a name prefix that
    // makes the per-instance scoping match production (production uses
    // the entity name implicitly via Component's scheduler instance; the
    // shim approximates by mixing the `this` pointer into the name).
    void set_timeout(const std::string& name, uint32_t delay_ms,
                     std::function<void()> cb);
    void set_interval(const std::string& name, uint32_t period_ms,
                      std::function<void()> cb);
    void cancel_timeout(const std::string& name);
    void cancel_interval(const std::string& name);

    // Returns an ESPPreferenceObject backed by the test's NVS slot store,
    // keyed by the entity name if this Component also inherits from
    // EntityBase. lora_client.cpp's LORAListener calls this from
    // restore_state_().
    template <typename T> ESPPreferenceObject make_entity_preference();
};

} // namespace esphome

#include "sim/sim_clock.h"

namespace esphome {

inline void Component::set_timeout(const std::string& name, uint32_t delay_ms,
                                   std::function<void()> cb) {
    auto* clk = shim_hooks::active_clock();
    if (!clk) return;
    char keyed[128];
    std::snprintf(keyed, sizeof(keyed), "%s@%p", name.c_str(), (void*)this);
    clk->set_timeout(keyed, delay_ms, std::move(cb));
}

inline void Component::set_interval(const std::string& name, uint32_t period_ms,
                                    std::function<void()> cb) {
    auto* clk = shim_hooks::active_clock();
    if (!clk) return;
    char keyed[128];
    std::snprintf(keyed, sizeof(keyed), "%s@%p", name.c_str(), (void*)this);
    clk->set_interval(keyed, period_ms, std::move(cb));
}

inline void Component::cancel_timeout(const std::string& name) {
    auto* clk = shim_hooks::active_clock();
    if (!clk) return;
    char keyed[128];
    std::snprintf(keyed, sizeof(keyed), "%s@%p", name.c_str(), (void*)this);
    clk->cancel_timeout(keyed);
}

inline void Component::cancel_interval(const std::string& name) {
    auto* clk = shim_hooks::active_clock();
    if (!clk) return;
    char keyed[128];
    std::snprintf(keyed, sizeof(keyed), "%s@%p", name.c_str(), (void*)this);
    clk->cancel_interval(keyed);
}

template <typename T>
ESPPreferenceObject Component::make_entity_preference() {
    // Try to fish the entity name out of EntityBase via dynamic_cast.
    auto* eb = dynamic_cast<EntityBase*>(this);
    std::string key = eb ? eb->get_name() : std::string{};
    if (key.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "comp@%p", (void*)this);
        key = buf;
    }
    return ESPPreferenceObject(shim_hooks::nvs_slot_for(key));
}

} // namespace esphome
