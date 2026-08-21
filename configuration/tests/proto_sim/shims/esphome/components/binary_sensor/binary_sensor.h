// Minimal binary_sensor shim.  lora_client.cpp uses one optional
// BinarySensor to surface "command failed" (F-4 tracked-op exhaustion) to
// Home Assistant; the harness only needs to observe published state.
#pragma once

#include "esphome/core/component.h"

namespace esphome {
namespace binary_sensor {

class BinarySensor : public EntityBase {
 public:
  void publish_state(bool state) {
    this->state = state;
    this->has_state_ = true;
  }
  bool has_state() const { return this->has_state_; }

  bool state{false};

 protected:
  bool has_state_{false};
};

}  // namespace binary_sensor
}  // namespace esphome
