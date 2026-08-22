#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace loracov
  {

    // P4b: Home Assistant control of automatic mode.
    //
    // Writing the switch updates the hub's desired mode, which reaches the node
    // either immediately (if it is awake, via a tracked sysop) or at its next
    // beacon (via the schedule push). The switch does NOT assume the node
    // complied: the node still refuses auto mode without a valid clock and a
    // schedule that can fire, and a physical button press flips it back to
    // interactive on its own. So the state is re-published from what the node
    // actually reports in its beacon, not from what was requested.
    class AutoModeSwitch : public switch_::Switch, public Component
    {
    public:
      void set_lora_parent(esphome::lora_tracker::LORAListener *parent) { this->parent_ = parent; }

      void setup() override;
      void dump_config() override;

    protected:
      void write_state(bool state) override;

      esphome::lora_tracker::LORAListener *parent_{nullptr};
    };

  } // namespace loracov
} // namespace esphome
