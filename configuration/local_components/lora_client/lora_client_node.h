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

      // P2: the node reported its own clock in a wake beacon; offset_s is
      // node_epoch - hub_epoch (positive = node runs ahead).  Default no-op so
      // only the platforms that surface it need to implement it.
      virtual void on_clock_offset(float offset_s) { (void) offset_s; }

      virtual void loop() {}
      void set_lora_client_parent(LORAListener *parent) { this->parent_ = parent; }

    protected:
      LORAListener *parent_{nullptr};
    };

  }
}