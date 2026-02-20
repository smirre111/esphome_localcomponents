#pragma once

#include "esphome/core/automation.h"
#include "esphome/components/lora_tracker/lora_tracker.h"

namespace esphome
{
  namespace lora_tracker
  {

    // placeholder class for static TAG .
    class Automation
    {
    public:
      // could be made inline with C++17
      static const char *const TAG;
    };

    template <typename... Ts>
    class SleepAction : public Action<Ts...>
    {
    public:
      explicit SleepAction(LORAListener *listener) : listener_(listener) {}

      void play(const Ts &...x) override { this->listener_->enterSleep(); }

    protected:
      LORAListener *listener_;
    };




    class SleepTrigger : public Trigger<const LORAListener &>, public LORAListener
    {
    public:
      explicit SleepTrigger(LORATracker *parent) { parent->register_listener(this); }
      void set_addresses(std::initializer_list<uint64_t> addresses) { this->address_vec_ = addresses; }

      bool parse_device(const LORAListener &device)
      {
        uint64_t u64_addr = device.address_uint64();
        if (!address_vec_.empty())
        {
          if (std::find(address_vec_.begin(), address_vec_.end(), u64_addr) == address_vec_.end())
          {
            return false;
          }
        }

        this->trigger(device);
        return true;
      }

    protected:
      std::vector<uint64_t> address_vec_;
    };

  } // namespace esphome::esp32_ble_tracker
}
