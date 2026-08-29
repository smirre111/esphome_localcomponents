#pragma once

#include "esphome/core/component.h"
#include "esphome/components/select/select.h"
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace loracov
  {
    // Action and day-mask for one schedule slot.
    //
    // Days are a SELECT with the same vocabulary the YAML already uses
    // (daily / weekdays / weekend / individual days) rather than seven
    // switches. Seven switches per slot would be 28 extra entities per node for
    // a choice that is almost always one of the first three.
    //
    // The trade-off is honest: arbitrary combinations like "Mon+Thu" are not
    // expressible here. The wire format carries a full 7-bit mask, so YAML can
    // still express them and this entity will show the closest preset — add a
    // "custom" path only if that need turns out to be real.

    enum class SlotSelectKind : uint8_t { ACTION, DAYS };

    class ScheduleSlotSelect : public select::Select, public Component
    {
    public:
      void set_lora_parent(esphome::lora_tracker::LORAListener *parent) { this->parent_ = parent; }
      void set_slot(uint8_t slot) { this->slot_ = slot; }
      void set_kind(SlotSelectKind kind) { this->kind_ = kind; }

      void setup() override;
      void dump_config() override;

      // bit0 = MON .. bit6 = SUN, matching ScheduleEntry.dayMask on the wire.
      static uint8_t mask_for_option(const std::string &opt);
      static const char *option_for_mask(uint8_t mask);

    protected:
      void control(const std::string &value) override;

      esphome::lora_tracker::LORAListener *parent_{nullptr};
      uint8_t slot_{0};
      SlotSelectKind kind_{SlotSelectKind::ACTION};
    };

  } // namespace loracov
} // namespace esphome
