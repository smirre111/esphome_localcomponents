#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lora_client/lora_client.h"

#include <array>
#include <string>
#include <vector>

namespace esphome
{
  namespace lora_tracker
  {

    class LORAListener;
    class LORAClient;

    class LORAClientNode
    {
    public:
      virtual void set_response(uint8_t *data, size_t len) = 0;
      virtual void send_remote_config() = 0;


      virtual void loop() {}
      void set_lora_client_parent(LORAListener *parent) { this->parent_ = parent; }

    protected:
      LORAListener *parent_{nullptr};
    };

  }
}