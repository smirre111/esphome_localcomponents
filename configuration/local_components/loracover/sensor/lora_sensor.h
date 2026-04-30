#pragma once

#include "esphome/core/component.h"

#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/loracover/loracover_base.h"

namespace esphome
{
  namespace loracov
  {

    class LoraCover : public esphome::lora_tracker::LORAClientNode, public PollingComponent
    {
    public:
      void setup() override;
      void update() override;
      // void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
      //                          esp_ble_gattc_cb_param_t *param) override;
      void set_response(uint8_t *data, size_t len) override;

      void dump_config() override;
      void set_battery(sensor::Sensor *battery) { battery_ = battery; }
      void set_voltage(sensor::Sensor *voltage) { voltage_ = voltage; }
      // void set_illuminance(sensor::Sensor *illuminance) { illuminance_ = illuminance; }

      void send_remote_config() override;
    protected:
      uint16_t char_handle_;
      std::unique_ptr<LoraCovEncoder> encoder_;
      std::unique_ptr<LoraCovDecoder> decoder_;
      // bool registered_;
      sensor::Sensor *battery_{nullptr};
      sensor::Sensor *voltage_{nullptr};
      // sensor::Sensor *illuminance_{nullptr};
      uint8_t current_sensor_;
      // The LoraCov often gets into a state where it spams loads of battery update
      // notifications. Here we will limit to no more than every 10s.
      uint8_t last_battery_update_;
    };

  } // namespace loracov
} // namespace esphome
