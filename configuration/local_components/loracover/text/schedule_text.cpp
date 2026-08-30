#include "schedule_text.h"

#include "esphome/core/log.h"

namespace esphome
{
  namespace loracov
  {
    static const char *const TAG = "loracov.schedule_text";

    void ScheduleText::publish_current_()
    {
      if (this->parent_ == nullptr)
        return;
      char buf[scheduletext::kTextLen];
      this->parent_->schedule_text(buf, sizeof(buf));
      this->publish_state(std::string(buf));
    }

    void ScheduleText::setup()
    {
      // Seed from the hub, which by now holds the persisted schedule or the YAML
      // seed. Showing a blank box and letting the user overwrite a live schedule
      // they could not see would be the worst possible default.
      this->publish_current_();
      if (this->status_ != nullptr)
        this->status_->publish_state("ok");

      // Home Assistant caps a state string at 255 characters, so this is a
      // reminder rather than the manual — the full grammar, the error list and
      // the reasons for the limits are in docs/schedule-text-syntax.md.
      if (this->help_ != nullptr)
        this->help_->publish_state(
            "HH:MM <days> <action>; ...  |  days: daily weekdays weekend "
            "mon..sun or mon,thu  |  action: open close stop position:0-100  |  "
            "max 8, empty clears");
    }

    void ScheduleText::control(const std::string &value)
    {
      if (this->parent_ == nullptr)
        return;

      char err[scheduletext::kErrorLen] = {0};
      const bool ok = this->parent_->set_schedule_text(value.c_str(), err, sizeof(err));

      if (this->status_ != nullptr)
        this->status_->publish_state(ok ? "ok" : (std::string("error: ") + err));

      // Publish what is ACTUALLY stored, not what was typed. On success that is
      // the canonical form (so `6:0 D Open` visibly becomes `06:00 daily open`);
      // on failure it is the unchanged schedule, so the box never shows a value
      // the hub is not running.
      this->publish_current_();

      if (!ok)
        ESP_LOGW(TAG, "Rejected: %s", err);
    }

    void ScheduleText::dump_config()
    {
      ESP_LOGCONFIG(TAG, "Schedule text");
    }

  } // namespace loracov
} // namespace esphome
