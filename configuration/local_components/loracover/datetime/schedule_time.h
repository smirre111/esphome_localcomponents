#pragma once

#include "esphome/core/component.h"
#include "esphome/components/datetime/time_entity.h"
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace loracov
  {
    // The time half of one schedule slot, editable from Home Assistant.
    //
    // Writing it updates sched_entries_[slot] on the hub, which marks the
    // schedule dirty; the node then sees a version mismatch in its next beacon
    // and the hub pushes. A sleeping node therefore does not pick the change up
    // immediately — up to checkin_interval — which the `Schedule Pending`
    // binary sensor already surfaces.
    //
    // restore_value is set in codegen so an HA edit survives a hub reboot. The
    // hub persists its own slot table too; both must agree, which is why the
    // entity publishes into the hub rather than the hub reading the entity.
    class ScheduleSlotTime : public datetime::TimeEntity, public Component
    {
    public:
      void set_lora_parent(esphome::lora_tracker::LORAListener *parent) { this->parent_ = parent; }
      void set_slot(uint8_t slot) { this->slot_ = slot; }

      void setup() override;
      void dump_config() override;

    protected:
      void control(const datetime::TimeCall &call) override;

      esphome::lora_tracker::LORAListener *parent_{nullptr};
      uint8_t slot_{0};
    };

  } // namespace loracov
} // namespace esphome
