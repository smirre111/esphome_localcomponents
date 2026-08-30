#include "schedule_position.h"

#include "esphome/core/log.h"

namespace esphome
{
  namespace loracov
  {
    static const char *const TAG = "loracov.schedule_position";

    void ScheduleSlotPosition::setup()
    {
      if (this->parent_ != nullptr)
        this->publish_state(this->parent_->slot(this->slot_).position_pct);
    }

    void ScheduleSlotPosition::control(float value)
    {
      const uint8_t pct = static_cast<uint8_t>(value < 0 ? 0 : (value > 100 ? 100 : value));
      this->publish_state(pct);
      if (this->parent_ != nullptr)
        this->parent_->set_slot_position(this->slot_, pct);
    }

    void ScheduleSlotPosition::dump_config()
    {
      ESP_LOGCONFIG(TAG, "Schedule slot %u position", (unsigned) this->slot_);
    }

  } // namespace loracov
} // namespace esphome
