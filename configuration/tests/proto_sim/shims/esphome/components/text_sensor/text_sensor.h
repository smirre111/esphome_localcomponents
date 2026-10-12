// Minimal text_sensor shim: lora_client.cpp publishes the node's sleep status
// ("awake" / "asleep until 05:00" / ...); the harness only observes the state
// and how many times it was published.
#pragma once

#include <string>

#include "esphome/core/component.h"

namespace esphome {
namespace text_sensor {

class TextSensor : public EntityBase {
 public:
  void publish_state(const std::string &state) {
    this->state = state;
    this->has_state_ = true;
    this->publishes++;
  }
  bool has_state() const { return this->has_state_; }

  std::string state;
  int publishes{0};

 protected:
  bool has_state_{false};
};

}  // namespace text_sensor
}  // namespace esphome
