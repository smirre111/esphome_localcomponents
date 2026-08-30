#pragma once

#include "esphome/core/component.h"
#include "esphome/components/text/text.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace loracov
  {
    // The whole schedule as one editable line:
    //   06:00 daily open; 21:45 daily close; 12:00 sat position:40
    //
    // Replaces twenty per-slot entities. What it gives up is that a picker
    // cannot be mistyped; what it must therefore guarantee is that a typo is
    // REJECTED rather than half-applied, and that the user is told why. The
    // published value is always the canonical rendering of what the hub really
    // holds, so a rejected edit visibly snaps back to the live schedule and the
    // companion status text says what was wrong.
    class ScheduleText : public text::Text, public Component
    {
    public:
      void set_lora_parent(esphome::lora_tracker::LORAListener *parent) { this->parent_ = parent; }
      void set_status(text_sensor::TextSensor *s) { this->status_ = s; }

      void setup() override;
      void dump_config() override;

    protected:
      void control(const std::string &value) override;
      void publish_current_();

      esphome::lora_tracker::LORAListener *parent_{nullptr};
      text_sensor::TextSensor *status_{nullptr};
    };

  } // namespace loracov
} // namespace esphome
