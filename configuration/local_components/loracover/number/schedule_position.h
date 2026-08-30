#pragma once

#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace loracov
  {
    // Target position for a slot whose action is `position`.
    //
    // Only meaningful for that action — open/close ignore it. It is still shown
    // unconditionally: hiding it behind the action would need the number to
    // watch the select, and a control that vanishes is worse than one that is
    // occasionally irrelevant.
    class ScheduleSlotPosition : public number::Number, public Component
    {
    public:
      void set_lora_parent(esphome::lora_tracker::LORAListener *parent) { this->parent_ = parent; }
      void set_slot(uint8_t slot) { this->slot_ = slot; }

      void setup() override;
      void dump_config() override;

    protected:
      void control(float value) override;

      esphome::lora_tracker::LORAListener *parent_{nullptr};
      uint8_t slot_{0};
    };

  } // namespace loracov
} // namespace esphome
