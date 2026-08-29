#include "schedule_enable_switch.h"

#include "esphome/core/log.h"

namespace esphome
{
  namespace loracov
  {
    static const char *const TAG = "loracov.schedule_enable";

    void ScheduleEnableSwitch::setup()
    {
      // Seed from the hub, which by now holds either the persisted schedule or
      // the YAML seed. The hub is the source of truth; the entity mirrors it.
      if (this->parent_ != nullptr)
        this->publish_state(this->parent_->slot(this->slot_).enabled);
    }

    void ScheduleEnableSwitch::write_state(bool state)
    {
      this->publish_state(state);
      if (this->parent_ != nullptr)
        this->parent_->set_slot_enabled(this->slot_, state);
    }

    void ScheduleEnableSwitch::dump_config()
    {
      ESP_LOGCONFIG(TAG, "Schedule slot %u enable", (unsigned) this->slot_);
    }

  } // namespace loracov
} // namespace esphome
