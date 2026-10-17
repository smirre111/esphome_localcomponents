// Minimal sensor shim (S6): the loracover sensor child publishes battery,
// voltage, RSSI, motor current and calibration telemetry; the harness observes
// the last state and how many times each sensor was published.
#pragma once

#include "esphome/core/component.h"

#define LOG_SENSOR(prefix, type, obj) do { (void) (obj); } while (0)

namespace esphome {
namespace sensor {

class Sensor : public EntityBase {
 public:
  void publish_state(float v) { this->state = v; this->publishes++; }
  float state{0.0f};
  int publishes{0};
};

}  // namespace sensor
}  // namespace esphome
