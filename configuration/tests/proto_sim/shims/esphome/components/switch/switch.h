// Minimal switch shim. lora_client.cpp holds one optional Switch (the
// automatic-mode toggle) and re-publishes its state from what the node reports
// in its wake beacon, so the harness only needs observable state.
#pragma once

#include "esphome/core/component.h"

namespace esphome {
namespace switch_ {

class Switch : public EntityBase {
 public:
  void publish_state(bool value) {
    this->state = value;
    this->has_state_ = true;
    this->publish_count_++;
  }
  bool has_state() const { return this->has_state_; }
  int publish_count() const { return this->publish_count_; }

  bool state{false};

 protected:
  bool has_state_{false};
  int  publish_count_{0};
};

}  // namespace switch_
}  // namespace esphome
