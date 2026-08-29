#include "schedule_select.h"

#include "esphome/core/log.h"

namespace esphome
{
  namespace loracov
  {
    static const char *const TAG = "loracov.schedule_select";

    // Keep these strings in step with the options list in __init__.py — the
    // Python side defines what HA offers, this side maps it to the wire value.
    static constexpr uint8_t kMaskWeekdays = 0b0011111;  // MON..FRI
    static constexpr uint8_t kMaskWeekend  = 0b1100000;  // SAT..SUN
    static constexpr uint8_t kMaskDaily    = 0b1111111;

    uint8_t ScheduleSlotSelect::mask_for_option(const std::string &opt)
    {
      if (opt == "daily")    return kMaskDaily;
      if (opt == "weekdays") return kMaskWeekdays;
      if (opt == "weekend")  return kMaskWeekend;
      if (opt == "mon") return 1 << 0;
      if (opt == "tue") return 1 << 1;
      if (opt == "wed") return 1 << 2;
      if (opt == "thu") return 1 << 3;
      if (opt == "fri") return 1 << 4;
      if (opt == "sat") return 1 << 5;
      if (opt == "sun") return 1 << 6;
      return kMaskDaily;
    }

    const char *ScheduleSlotSelect::option_for_mask(uint8_t mask)
    {
      switch (mask)
      {
      case kMaskDaily:    return "daily";
      case kMaskWeekdays: return "weekdays";
      case kMaskWeekend:  return "weekend";
      case 1 << 0: return "mon";
      case 1 << 1: return "tue";
      case 1 << 2: return "wed";
      case 1 << 3: return "thu";
      case 1 << 4: return "fri";
      case 1 << 5: return "sat";
      case 1 << 6: return "sun";
      default: break;
      }
      // A mask YAML can express but this select cannot (e.g. Mon+Thu). Show the
      // nearest safe label rather than an empty entity; editing the select will
      // then overwrite it, which is the honest behaviour — the alternative is a
      // control whose displayed value is a lie.
      return "daily";
    }

    void ScheduleSlotSelect::setup()
    {
      if (this->parent_ == nullptr)
        return;
      const auto &e = this->parent_->slot(this->slot_);
      if (this->kind_ == SlotSelectKind::ACTION)
        this->publish_state(e.action == 1 ? "close" : (e.action == 2 ? "position" : "open"));
      else
        this->publish_state(option_for_mask(e.day_mask));
    }

    void ScheduleSlotSelect::control(const std::string &value)
    {
      this->publish_state(value);
      if (this->parent_ == nullptr)
        return;

      if (this->kind_ == SlotSelectKind::ACTION)
      {
        const uint8_t action = (value == "close") ? 1 : (value == "position" ? 2 : 0);
        this->parent_->set_slot_action(this->slot_, action);
      }
      else
      {
        this->parent_->set_slot_days(this->slot_, mask_for_option(value));
      }
    }

    void ScheduleSlotSelect::dump_config()
    {
      ESP_LOGCONFIG(TAG, "Schedule slot %u %s", (unsigned) this->slot_,
                    this->kind_ == SlotSelectKind::ACTION ? "action" : "days");
    }

  } // namespace loracov
} // namespace esphome
