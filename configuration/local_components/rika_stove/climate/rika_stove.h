#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/climate/climate.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include <sstream>
#include <queue>

namespace esphome
{
  namespace rikastove
  {

#define RIKASTOVE_READ_BUFFER_LENGTH 255

    enum State
    {
      STATE_IDLE = 0,
      STATE_SEND_SMS
    };

    enum State1
    {
      STATE_INIT = 0,
      READ_STOVE_AT_COMMAND,
      STOVE_AT_COMMAND_COMPLETE,
      READ_STOVE_OUTGOING_SMS,
      STOVE_OUTGOING_SMS_COMPLETE,
      READ_STOVE_STATUS,
      STOVE_STATUS_COMPLETE

    };

    enum AT_Command
    {
      AT,
      ATE,
      CNMI,
      CMGF,
      IPR,
      ATF,
      CMGD,
      CMGR,
      CMGS,
      UNKNOWN
    };

    class RikaStove : public climate::Climate, public uart::UARTDevice, public Component
    {
    public:
      void setup() override;

      // If a polling component
      // void update() override;
      // If a normal component
      void update();
      void loop() override;
      void dump_config() override;

      void set_supports_cool(bool supports_cool) { this->supports_cool_ = supports_cool; }
      void set_supports_heat(bool supports_heat) { this->supports_heat_ = supports_heat; }
      void set_phone_number(std::string phone_number) { this->phone_number_ = phone_number; }
      void set_pin_code(std::string pin_code) { this->pin_code_ = pin_code; }

    protected:
      /// Override control to change settings of the climate device.
      void control(const climate::ClimateCall &call) override;
      /// Return the traits of this controller.
      climate::ClimateTraits traits() override;

      // Schedule transmission of the state of this climate controller.
      void encode_stove_cmd_(bool mode_change, bool temp_change);

      AT_Command parse_command_(std::string const &command) const;

      std::string state_to_string_(State1 state) const;

      void parse_buffer_(const std::string &);

      void parse_reply_(const std::string &);

      void delete_sms_();

      void send_query_(int msg_num);

      void send_crlf_();
      void send_ok_();
      void send_error_();
      void set_state(State1 state);

#ifdef USE_TEXT_SENSOR
    public:
      enum class SubTextSensorType
      {
        STOVE_STATUS = 0,
        SUB_TEXT_SENSOR_TYPE_COUNT,
      };
      void set_sub_text_sensor(SubTextSensorType type, text_sensor::TextSensor *sens);

    protected:
      void update_sub_text_sensor_(SubTextSensorType type, const std::string &value);
      text_sensor::TextSensor *sub_text_sensors_[(size_t)SubTextSensorType::SUB_TEXT_SENSOR_TYPE_COUNT]{nullptr};
#endif

      State state_{STATE_IDLE};
      State1 state1_{STATE_INIT};

      // std::string status_message_{""};
      std::queue<std::string> command_queue_;

      // Message queue to hold incoming messages from stove
      std::queue<std::string> msg_queue_;

      char read_buffer_[RIKASTOVE_READ_BUFFER_LENGTH];
      size_t read_pos_{0};
      uint8_t parse_index_{0};
      bool initialized_{false};
      bool query_status_{false};

      bool supports_cool_{false};
      bool supports_heat_{true};

      std::string phone_number_{"+436508012415"};
      std::string pin_code_{"1211"};

      bool echo_{false};
      const char ASCII_CR{0x0D};
      const char ASCII_LF{0x0A};
      const char ASCII_ESC{0x1A};

      char term_char_{ASCII_CR};

      std::string status_message_{""};
      int message_counter_{0};
      
    };

  } // namespace rikastove
} // namespace esphome
