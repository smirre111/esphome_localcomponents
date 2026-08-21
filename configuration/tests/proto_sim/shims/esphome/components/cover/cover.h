// Host-side shim for ESPHome's cover component. Only the surface
// LoraCoverComponent touches is provided: position/target_position_,
// publish_state, restore_state_, control(), get_traits(), CoverCall and
// CoverTraits. The real Cover base also exposes friendly_name, HA event
// triggers, etc — out of scope for protocol tests.
#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace esphome::cover {

constexpr float COVER_OPEN  = 1.0f;
constexpr float COVER_CLOSED = 0.0f;

class CoverTraits {
public:
    void set_supports_stop(bool v)         { stop_ = v; }
    void set_supports_position(bool v)     { position_ = v; }
    void set_supports_tilt(bool v)         { tilt_ = v; }
    void set_supports_toggle(bool v)       { toggle_ = v; }
    void set_is_assumed_state(bool v)      { assumed_ = v; }
    bool get_supports_stop() const         { return stop_; }
    bool get_supports_position() const     { return position_; }
private:
    bool stop_{false}, position_{false}, tilt_{false}, toggle_{false}, assumed_{false};
};

class Cover;

class CoverCall {
public:
    CoverCall& set_stop(bool v)            { stop_ = v; return *this; }
    CoverCall& set_position(float p)       { position_ = p; return *this; }
    CoverCall& set_toggle(bool v)          { toggle_ = v; return *this; }
    bool get_stop() const                  { return stop_; }
    std::optional<float> get_position() const { return position_; }
    std::optional<bool>  get_toggle() const   { return toggle_; }
private:
    bool stop_{false};
    std::optional<float> position_;
    std::optional<bool>  toggle_;
};

// Snapshot of restored state — production stores per-cover position in
// NVS via restore_state_(). Tests can pre-populate by injecting through
// the NVS slot store the Component shim already provides.
struct CoverRestoreState {
    float position{0.5f};
    // Production calls restore->apply(this) to copy into the Cover.
    void apply(Cover* c);
};

// Production cover::Cover inherits EntityBase and (transitively) Component
// via Nameable; our shim only inherits EntityBase because LoraCoverComponent
// already adds `, public Component` itself — avoids the diamond.
class Cover : public esphome::EntityBase {
public:
    Cover() = default;

    // The state the firmware exposes to HA. Public to match production.
    float position{0.5f};

    // Production calls publish_state(force=true) after changing position.
    // The shim records the latest published value for inspection.
    void publish_state(bool force = false) {
        (void)force;
        last_published_ = position;
        ++publish_count_;
    }

    // Tests can pre-load a restored state by writing to the shim's NVS
    // slot store keyed by the entity name (Component::make_entity_preference
    // already uses entity name as the key). For phase-3 we keep this
    // minimal: restore returns nullopt unless something was injected via
    // inject_restore_state().
    esphome::optional<CoverRestoreState> restore_state_() {
        if (restore_.has_value()) return restore_;
        return esphome::optional<CoverRestoreState>{};
    }

    // Test inspection.
    int  publish_count() const { return publish_count_; }
    float last_published() const { return last_published_; }
    void inject_restore_state(CoverRestoreState s) { restore_ = s; }

    // Production exposes traits + control as pure virtuals on the real
    // Cover class. We keep them virtual but non-pure so a minimal shim
    // user (without subclass) still compiles.
    virtual CoverTraits get_traits() { return {}; }

protected:
    virtual void control(const CoverCall& /*call*/) {}

private:
    int   publish_count_{0};
    float last_published_{0.5f};
    std::optional<CoverRestoreState> restore_;
};

inline void CoverRestoreState::apply(Cover* c) {
    if (c) c->position = position;
}

} // namespace esphome::cover

// LOG_COVER macro used by Cover dump_config(). No-op shim.
#define LOG_COVER(prefix, type, obj) do { (void)(prefix); (void)(type); (void)(obj); } while (0)
