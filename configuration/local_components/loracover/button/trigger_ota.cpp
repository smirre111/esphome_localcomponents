#include "trigger_ota.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"


namespace esphome
{
  namespace loracov
  {

    static const char *const TAG = "loracov_ota_button";

    void TriggerOtaButton::dump_config() 
    {
      ESP_LOGCONFIG(TAG, "LORA_OTA_BUTTON");
      LOG_BUTTON("", "OTA Button", this);
    }

    void TriggerOtaButton::press_action()
    {
      this->parent_->triggerOTA();
    }

    void TriggerOtaButton::send_remote_config()
    {
    }

    void TriggerOtaButton::set_response(uint8_t *data, size_t len)
    {
    }

  } // namespace loracov
} // namespace esphome
