#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace loracov
  {
    // Enable/disable one schedule slot without losing what is in it.
    //
    // Turning a slot off is the common case for a seasonal entry — losing the
    // time and days as the price of pausing it would make the feature annoying
    // enough to go unused. Disabled slots are omitted from the pushed frame, so
    // a paused slot costs no airtime (~7 B per entry in a frame whose 17-copy
    // burst already fills half its round).
    class ScheduleEnableSwitch : public switch_::Switch, public Component
    {
    public:
      void set_lora_parent(esphome::lora_tracker::LORAListener *parent) { this->parent_ = parent; }
      void set_slot(uint8_t slot) { this->slot_ = slot; }

      void setup() override;
      void dump_config() override;

    protected:
      void write_state(bool state) override;

      esphome::lora_tracker::LORAListener *parent_{nullptr};
      uint8_t slot_{0};
    };

  } // namespace loracov
} // namespace esphome
