#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace loracov {

static const uint16_t LORA_COVER_SERVICE_UUID = 0xFE50;
static const uint16_t LORA_COVER_CHARACTERISTIC_UUID = 0xFE51;
//
// Tuya identifiers, only to detect and warn users as they are incompatible.
static const uint16_t LORA_COVER_TUYA_SERVICE_UUID = 0x1910;
static const uint16_t LORA_COVER_TUYA_CHARACTERISTIC_UUID = 0x2b11;

struct LoraCovPacket {
  uint8_t length;
  uint8_t data[24];
};

static const uint8_t CMD_GET_BATTERY_LEVEL = 0xA2;
static const uint8_t CMD_GET_LIGHT_LEVEL = 0xAA;
static const uint8_t CMD_GET_POSITION = 0xA7;
static const uint8_t CMD_SEND_PIN = 0x17;
static const uint8_t CMD_SET_STATE = 0x0A;
static const uint8_t CMD_SET_POSITION = 0x0D;
static const uint8_t CMD_NOTIFY_POSITION = 0xA1;

static const uint8_t RESPONSE_ACK = 0x5A;
static const uint8_t RESPONSE_NACK = 0xA5;

class LoraCovEncoder {
 public:
  LoraCovPacket *get_battery_level_request();
  LoraCovPacket *get_light_level_request();
  LoraCovPacket *get_position_request();
  LoraCovPacket *get_send_pin_request(uint16_t pin);
  LoraCovPacket *get_open_request();
  LoraCovPacket *get_close_request();
  LoraCovPacket *get_stop_request();
  LoraCovPacket *get_set_position_request(uint8_t position);

 protected:
  void checksum_();
  LoraCovPacket *encode_(uint8_t command, uint8_t *data, uint8_t length);
  LoraCovPacket packet_;
};

class LoraCovDecoder {
 public:
  void decode(const uint8_t *data, uint16_t length);
  bool has_battery_level() { return this->has_battery_level_; }
  bool has_light_level() { return this->has_light_level_; }
  bool has_set_position_response() { return this->has_set_position_response_; }
  bool has_set_state_response() { return this->has_set_state_response_; }
  bool has_position() { return this->has_position_; }
  bool has_pin_response() { return this->has_pin_response_; }

  union {
    uint8_t position_;
    uint8_t battery_level_;
    float light_level_;
    uint8_t set_position_ok_;
    uint8_t set_state_ok_;
    uint8_t pin_ok_;
  };

 protected:
  bool has_battery_level_;
  bool has_light_level_;
  bool has_set_position_response_;
  bool has_set_state_response_;
  bool has_position_;
  bool has_pin_response_;
};

}  // namespace LoraCov
}  // namespace esphome
