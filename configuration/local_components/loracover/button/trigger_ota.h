#pragma once

#include "esphome/components/button/button.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/loracover/loracover_base.h"

namespace esphome
{
  namespace loracov
  {

    class TriggerOtaButton : public esphome::lora_tracker::LORAClientNode, public button::Button, public Component
    {
    public:
      TriggerOtaButton() = default;

      void set_response(uint8_t *data, size_t len) override;

      void dump_config() override;

      void send_remote_config() override;

    protected:
      void press_action() override;
    };

  } // namespace loracov
} // namespace esphome
