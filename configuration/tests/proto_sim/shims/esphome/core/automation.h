// Host-side shim — lora_client.h includes this but only uses the namespace.
// Triggers/Actions are referenced from the `automation.h` of automation
// hooks; lora_client doesn't define any in the .cpp. Empty stub suffices
// for compile pass.
#pragma once
#include <functional>
#include <vector>

namespace esphome::automation {

template <typename... Ts>
class Trigger {
public:
    virtual ~Trigger() = default;
    void trigger(Ts... /*args*/) {}
};

template <typename... Ts>
class Action {
public:
    virtual ~Action() = default;
    virtual void play(Ts... args) = 0;
};

} // namespace esphome::automation
