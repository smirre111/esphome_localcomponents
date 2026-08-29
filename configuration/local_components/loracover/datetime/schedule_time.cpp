#include "schedule_time.h"

#include "esphome/core/log.h"

namespace esphome
{
  namespace loracov
  {
    static const char *const TAG = "loracov.schedule_time";

    void ScheduleSlotTime::setup()
    {
      // Seed the entity from whatever the hub currently holds — which after
      // restore_schedule_() is the persisted value, or the YAML seed on first
      // boot. Without this the entity would show 00:00 until someone edited it,
      // while the hub quietly ran a different time.
      if (this->parent_ == nullptr)
        return;
      const auto &e = this->parent_->slot(this->slot_);
      this->hour_   = e.minute_of_day / 60;
      this->minute_ = e.minute_of_day % 60;
      this->second_ = 0;
      this->publish_state();
    }

    void ScheduleSlotTime::control(const datetime::TimeCall &call)
    {
      if (call.get_hour().has_value())
        this->hour_ = *call.get_hour();
      if (call.get_minute().has_value())
        this->minute_ = *call.get_minute();
      this->second_ = 0;   // the schedule has minute resolution

      this->publish_state();
      if (this->parent_ != nullptr)
        this->parent_->set_slot_minute(this->slot_,
                                       static_cast<uint16_t>(this->hour_) * 60 + this->minute_);
    }

    void ScheduleSlotTime::dump_config()
    {
      ESP_LOGCONFIG(TAG, "Schedule slot %u time: %02u:%02u",
                    (unsigned) this->slot_, (unsigned) this->hour_, (unsigned) this->minute_);
    }

  } // namespace loracov
} // namespace esphome
