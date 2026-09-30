#include "debug_sysop_button.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"


namespace esphome
{
  namespace loracov
  {

    static const char *const TAG = "loracov_debug_sysop_button";

    void DebugSysopButton::dump_config()
    {
      ESP_LOGCONFIG(TAG, "LORA_DEBUG_SYSOP_BUTTON op=%d", (int) this->op_);
      LOG_BUTTON("", "Debug Sysop Button", this);
    }

    void DebugSysopButton::press_action()
    {
      this->parent_->sendDebugSysop(this->op_);
    }

    void DebugSysopButton::send_remote_config()
    {
    }

    void DebugSysopButton::set_response(uint8_t *data, size_t len)
    {
    }

  } // namespace loracov
} // namespace esphome
