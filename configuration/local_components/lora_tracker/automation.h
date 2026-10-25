#pragma once

#include "esphome/core/automation.h"
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace lora_tracker
  {

    template <typename... Ts>
    class SleepAction : public Action<Ts...>
    {
    public:
      explicit SleepAction(LORAListener *listener) : listener_(listener) {}

      void play(const Ts &...x) override { this->listener_->enterSleep(); }

    protected:
      LORAListener *listener_;
    };

  }  // namespace lora_tracker
}
