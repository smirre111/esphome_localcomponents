#include "auto_mode_switch.h"
#include "esphome/core/log.h"

namespace esphome
{
  namespace loracov
  {

    static const char *const TAG = "loracov.auto_mode";

    void AutoModeSwitch::setup()
    {
      if (this->parent_ == nullptr)
        return;
      // Publish the hub's configured default so Home Assistant does not show a
      // blank toggle until the node happens to beacon.
      this->publish_state(this->parent_->get_auto_mode());
    }

    void AutoModeSwitch::dump_config()
    {
      LOG_SWITCH("", "LoRa Auto Mode", this);
    }

    void AutoModeSwitch::write_state(bool state)
    {
      if (this->parent_ == nullptr)
        return;
      ESP_LOGI(TAG, "Auto mode requested: %s", state ? "ON" : "OFF");
      this->parent_->set_auto_mode(state);
      // Optimistic: reflect the request now, then correct it from the node's
      // reported mode at the next beacon if it did not (or could not) comply.
      this->publish_state(state);
    }

  } // namespace loracov
} // namespace esphome
