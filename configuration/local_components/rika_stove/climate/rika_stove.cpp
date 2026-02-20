#include "rika_stove.h"
#include "esphome/core/log.h"
#include <esp_system.h>
#include <Arduino.h>
#include <ctime>

#define SUPPORTS_STATUS 0

namespace esphome
{
  namespace rikastove
  {

    static const char *TAG = "rikastove.climate";

    // const uint32_t COOLIX_OFF = 0xB27BE0;

    // const uint8_t COOLIX_COOL = 0b00;
    // const uint8_t COOLIX_DRY = 0b01;
    // const uint8_t COOLIX_AUTO = 0b10;
    // const uint8_t COOLIX_HEAT = 0b11;

    const float POWER_MIN = 30.0;
    const float POWER_MAX = 100.0;
    const uint8_t POWER_STEP = 5;
    constexpr size_t MAX_COMMAND_QUEUE = 5;
    constexpr size_t MAX_MSG_QUEUE = 5;

    climate::ClimateTraits RikaStove::traits()
    {
      auto traits = climate::ClimateTraits();
      // traits.set_supports_current_temperature(true);
      traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
      // traits.set_supports_auto_mode(true);
      traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT});
      // traits.set_supports_cool_mode(this->supports_cool_);
      // traits.set_supports_heat_mode(this->supports_heat_);
      // traits.set_supports_two_point_target_temperature(false);
      // traits.set_supports_away(false);
      traits.set_visual_min_temperature(17);
      traits.set_visual_max_temperature(30);
      traits.set_visual_temperature_step(1);
      return traits;
    }

    void RikaStove::dump_config()
    {
      // ESP_LOGCONFIG(TAG, "Stove '%s':", this->stove_->get_name().c_str());
      ESP_LOGCONFIG(TAG, "rika_stove:");
      ESP_LOGCONFIG(TAG, "  pin: %s", this->pin_code_.c_str());
      ESP_LOGCONFIG(TAG, "  phone_number: %s", this->phone_number_.c_str());
    }

    void RikaStove::setup()
    {
      ESP_LOGD(TAG, "Setting up Rika Stove Climate Controller");
      this->current_temperature = 20.0f;
      // restore set points
      auto restore = this->restore_state_();
      if (restore.has_value())
      {
        restore->apply(this);
      }
      else
      {
        ESP_LOGD(TAG, "No restore data found, setting defaults");
        // restore from defaults
        this->mode = climate::CLIMATE_MODE_OFF;
        // initialize target temperature to some value so that it's not NAN
        this->target_temperature = roundf(this->current_temperature);
      }

      // The first message that we will exchange with stove is the TEL command to be able
      // to receive messages from the stove
      this->command_queue_.push("TEL");
    }

    void RikaStove::control(const climate::ClimateCall &call)
    {
      bool mode_change = false;
      bool temp_change = false;
      if (call.get_mode().has_value())
      {
        this->mode = *call.get_mode();
        mode_change = true;
      }

      if (call.get_target_temperature().has_value())
      {
        this->target_temperature = *call.get_target_temperature();
        temp_change = true;
      }
      // Tell the stove that there is new state
      this->encode_stove_cmd_(mode_change, temp_change);

      // Publish the state through native API
      this->publish_state();
    }

    void RikaStove::update()
    {
      ESP_LOGD(TAG, "In update");
      // In case we want add periodic reading of the state / temperature using a polling component
      // this function will be called periodically by the core
    }

    void RikaStove::loop()
    {
      // Read message
      while (this->available())
      {
        uint8_t byte;
        this->read_byte(&byte);

        if (byte >= 0x7F)
        {
          byte = '?'; // need to be valid utf8 string for log functions.
        }

        this->read_buffer_[this->read_pos_] = byte;

        if ((byte == this->term_char_) || (byte == this->ASCII_LF) || (byte == this->ASCII_ESC) || (byte == this->ASCII_CR))
        {
          this->read_buffer_[this->read_pos_] = 0;
          this->read_pos_ = 0;
          if (this->msg_queue_.size() < MAX_MSG_QUEUE)
            this->msg_queue_.push(std::string(this->read_buffer_));
          else
            ESP_LOGW(TAG, "msg_queue_ full, dropping incoming message");
        }
        else
        {
          this->read_pos_++;
          if (this->read_pos_ == RIKASTOVE_READ_BUFFER_LENGTH)
          {
            this->read_pos_ = 0;
          }
        }
      }
      if (!this->msg_queue_.empty())
      {
        std::string comm_buffer = this->msg_queue_.front();
        this->msg_queue_.pop();
        this->parse_buffer_(comm_buffer);
      }

      // Periodic free heap logging every 60s
      {
        time_t now = time(NULL);
        static time_t last_heap_log = 0;
        if (now != (time_t)-1 && (now - last_heap_log) >= 60)
        {
          last_heap_log = now;
          ESP_LOGD(TAG, "Free heap: %u", (unsigned)esp_get_free_heap_size());
        }
      }

#if (SUPPORTS_STATUS == 1)
      this->message_counter_++;

      // If stove supports status reading, request it every 20 messages
      if (this->message_counter_ > 20)
      {
        this->message_counter_ = 0;
        this->set_state(State1::READ_STOVE_STATUS);
        this->command_queue_.push("?");
      }
#endif
    }

    void RikaStove::encode_stove_cmd_(bool mode_change, bool temp_change)
    {

      std::string mode_string;
      std::string mode_short;

      ESP_LOGD(TAG, "Vmin '%f:", this->traits().get_visual_min_temperature());
      ESP_LOGD(TAG, "Vmax '%f:", this->traits().get_visual_max_temperature());

      switch (this->mode)
      {
      case climate::CLIMATE_MODE_COOL:
        mode_string = "ROOM";
        mode_short = "r";
        break;
      case climate::CLIMATE_MODE_HEAT:
        mode_string = "HEAT";
        mode_short = "h";
        break;
      case climate::CLIMATE_MODE_AUTO:
        mode_short = "AUTO";
        mode_string = "AUTO";
        break;
      case climate::CLIMATE_MODE_OFF:
      default:
        mode_short = "OFF";
        mode_string = "OFF";
        break;
      }

      // In HEAT mode we have to supply a power value between 30% and 100%
      uint8_t temp;
      temp = (uint8_t)roundf(clamp(this->target_temperature, this->traits().get_visual_min_temperature(), this->traits().get_visual_max_temperature()));

      // Convert the temperature for the HEAT mode to a per cent value in power
      float power = (temp - this->traits().get_visual_min_temperature()) * ((POWER_MAX - POWER_MIN) / (this->traits().get_visual_max_temperature() - this->traits().get_visual_min_temperature())) + POWER_MIN;

      // The power level needs to be set in 5% steps from 30 to 100%
      temp = ((uint8_t)roundf(power / POWER_STEP)) * POWER_STEP;

      // The temperature/power level needs to be converted into a string
      std::ostringstream ss;
      std::string temp_str;
      ss << (int)temp;
      temp_str = ss.str();

      std::string api_command;

      if (mode_change == true)
      {
        api_command = mode_string;
      }
      if (temp_change == true)
      {
        api_command = mode_short + temp_str;
      }

      ESP_LOGD(TAG, "Adding API command to queue: %s", api_command.c_str());
      if (!api_command.empty())
      {
        if (this->command_queue_.size() >= MAX_COMMAND_QUEUE)
        {
          ESP_LOGW(TAG, "command_queue_ full (%zu), dropping oldest command", this->command_queue_.size());
          this->command_queue_.pop();
        }
        this->command_queue_.push(api_command);
      }
    }

    void RikaStove::delete_sms_()
    {

      if (!this->command_queue_.empty())
      {
        ESP_LOGD(TAG, "-> Deleted message: %s", command_queue_.front().c_str());
        this->command_queue_.pop();
      }
    }

    void RikaStove::send_query_(int msg_num)
    {

      char output[50];
      time_t timestamp = time(NULL);
      struct tm datetime = *localtime(&timestamp);

      strftime(output, 50, "%y/%m/%d,%H:%M:%S+00", &datetime);

      std::string message;
      // message = "+CMGR: " + std::to_string(msg_num) + ",";
      // message += "\"REC READ\",";
      message = "+CMGR: \"REC READ\",";
      message += "\"" + this->phone_number_ + "\",";
      message += ",";
      message += "\"" + std::string(output) + "\"";

      message.reserve(128);
      this->write_str(message.c_str());
      this->send_crlf_();
      delay(0);

      ESP_LOGD(TAG, "-> Stove CMD: Read SMS");
      ESP_LOGD(TAG, "-> %s", message.c_str());

      if (this->command_queue_.empty())
      {
        ESP_LOGW(TAG, "No command to send when handling AT+CMGR");
        return;
      }

      message = this->pin_code_ + " " + this->command_queue_.front();

      this->write_str(message.c_str());
      this->send_crlf_();
      delay(0);

      ESP_LOGD(TAG, "-> %s", message.c_str());

      //// Pop the command after sending to avoid re-sending and unbounded growth
      //this->command_queue_.pop();
    }

    void RikaStove::parse_buffer_(const std::string &comm_buffer)
    {

      ESP_LOGD(TAG, "Received comm_buffer: %s", comm_buffer.c_str());
      if (comm_buffer.empty())
      {
        this->state_ = STATE_IDLE;
        this->term_char_ = ASCII_CR;
        return;
      }

      AT_Command command = this->parse_command_(comm_buffer);

      switch (this->state_)
      {

      case STATE_IDLE:
      {
        // DELETE_MESSAGE
        if (comm_buffer.rfind("AT+CMGD", 0) == 0)
        { // The stove asks to delete the current SMS
          ESP_LOGD(TAG, "-> Stove CMD: Delete SMS");

          this->delete_sms_();
          this->send_ok_();
          this->set_state(State1::STATE_INIT);
        }
        // ECHO OFF
        else if (comm_buffer.rfind("ATE", 0) == 0)
        {
          ESP_LOGD(TAG, "-> Stove CMD: ATE0");
          this->echo_ = (comm_buffer[3] == '1') ? true : false;
          this->send_ok_();
        }
        // STATE NEW_MESSAGE
        else if (comm_buffer.rfind("AT+CNMI", 0) == 0)
        {
          ESP_LOGD(TAG, "-> Stove CMD: New SMS");
          this->send_ok_();
        }
        // STATE MESSAGE FORMAT
        else if (comm_buffer.rfind("AT+CMGF", 0) == 0)
        {
          ESP_LOGD(TAG, "-> Stove CMD: Message Format");
          this->send_ok_();
        }
        // STATE SEND SMS
        else if (comm_buffer.rfind("AT+CMGS", 0) == 0)
        {
          ESP_LOGD(TAG, "-> Stove CMD: Send SMS");
          this->send_crlf_();
          this->write_str("> ");

          // Termination charactor for message is switched to ESC
          this->term_char_ = ASCII_ESC;
          this->state_ = STATE_SEND_SMS;
          this->set_state(State1::READ_STOVE_OUTGOING_SMS);
        }
        // STATE SMS RECEIVED
        else if (comm_buffer.rfind("AT+CMGR", 0) == 0)
        { // The stove wants to read an SMS

          ESP_LOGD(TAG, "-> Stove CMD: Read SMS");

          if (this->command_queue_.empty() == false)
          {
            // There is a command to send to the stove
            this->send_query_(1);
            this->query_status_ = true;
          }
          else
          {
            // There is no SMS command to send to the stove
            this->send_crlf_();
            this->send_ok_();
            this->set_state(State1::STATE_INIT);
            ESP_LOGD(TAG, "-> %s", "No pending SMS command");
          }
        }
        else if ((comm_buffer != "") && (comm_buffer != "\n") && (comm_buffer != "\x1A") && (comm_buffer != "\x0D"))
        {
          this->send_error_();
        }
        break;
      }

      case STATE_SEND_SMS:
      {
        if (this->state1_ == State1::READ_STOVE_STATUS)
        {
          ESP_LOGD(TAG, "-> Stove CMD: Read Status");
          update_sub_text_sensor_(SubTextSensorType::STOVE_STATUS, comm_buffer);

          this->term_char_ = ASCII_CR;
          this->state_ = STATE_IDLE;
          this->set_state(State1::STATE_INIT);
        }
        else
        {

          this->parse_reply_(comm_buffer);
          // Send the response
          this->send_crlf_();
          this->write_str("+CMGS: 01");
          this->send_crlf_();
          this->send_ok_();

          this->term_char_ = ASCII_CR;
          this->state_ = STATE_IDLE;
          this->set_state(State1::STATE_INIT);
          update_sub_text_sensor_(SubTextSensorType::STOVE_STATUS, comm_buffer);
        }
        break;
      }

      default:
      {
        this->term_char_ = ASCII_CR;
        this->state_ = STATE_IDLE;
        this->set_state(State1::STATE_INIT);
        break;
      }
      }
    }

    void RikaStove::parse_reply_(const std::string &stove_reply)
    {
      size_t pos;

      // STOVE ON - HEAT 30
      // STOVE ON - HEAT 100
      // STOVE ON - ROOM 22
      // STOVE OFF
      // FAILURE: code error
      // FAILURE: text error
      std::istringstream is(stove_reply);
      std::string part;
      std::vector<std::string> str_vec;
      while (getline(is, part, ' '))
      {
        str_vec.push_back(part);
      }

      std::string rply;
      std::string mode;
      std::string value;

      rply = str_vec[0];

      if (rply == "STOVE")
      {
        if (str_vec[1] == "ON")
        {
          mode = str_vec[3];
          if (str_vec.size() > 4)
          {
            value = str_vec[4];
            std::stringstream ss(value);
            int temp_int = 0;
            ss >> temp_int;
            // For HEAT mode we need to re-scale power into temperature
            if (mode == "HEAT")
            {
              float power = (temp_int - POWER_MIN) * ((this->traits().get_visual_max_temperature() - this->traits().get_visual_min_temperature()) / (POWER_MAX - POWER_MIN)) + this->traits().get_visual_min_temperature();
              temp_int = (uint8_t)roundf(power);
            }
            this->current_temperature = (float)temp_int;
            this->publish_state();
          }

          ESP_LOGD(TAG, "Stove reply: OK: %s", stove_reply.c_str());
        }
        else if (str_vec[1] == "OFF")
        {
          ESP_LOGD(TAG, "Stove reply: OK: %s", stove_reply.c_str());
        }
        else
        {
          ESP_LOGW(TAG, "Stove reply: UNKNOWN: %s", stove_reply.c_str());
        }
      }
      else if (rply == "FAILURE:")
      {
        ESP_LOGE(TAG, "Stove reply: FAILURE: %s", stove_reply.c_str());
      }
      else
      {
        ESP_LOGI(TAG, "Stove reply: UNKNOWN: %s", stove_reply.c_str());
      }
    }

    AT_Command RikaStove::parse_command_(std::string const &command) const
    {
      if (esphome::str_startswith(command, "AT+CMGR"))
        return AT_Command::CMGR;
      if (esphome::str_startswith(command, "AT+CMGS"))
        return AT_Command::CMGS;
      if (esphome::str_startswith(command, "AT+CMGD"))
        return AT_Command::CMGD;
      if (command == "AT")
        return AT_Command::AT;
      if (command == "AT&F")
        return AT_Command::ATF;
      if (esphome::str_startswith(command, "AT+CNMI"))
        return AT_Command::CNMI;
      if (esphome::str_startswith(command, "AT+CMGF"))
        return AT_Command::CMGF;
      if (esphome::str_startswith(command, "AT+IPR"))
        return AT_Command::IPR;
      if (esphome::str_startswith(command, "ATE"))
        return AT_Command::ATE;

      return AT_Command::UNKNOWN;
    }

    std::string RikaStove::state_to_string_(State1 state) const
    {
      switch (state)
      {
      case State1::STATE_INIT:
        return "STATE_INIT";
      case State1::READ_STOVE_AT_COMMAND:
        return "READ_STOVE_AT_COMMAND";
      case State1::STOVE_AT_COMMAND_COMPLETE:
        return "STOVE_AT_COMMAND_COMPLETE";
      case State1::READ_STOVE_OUTGOING_SMS:
        return "READ_STOVE_OUTGOING_SMS";
      case State1::STOVE_OUTGOING_SMS_COMPLETE:
        return "STOVE_OUTGOING_SMS_COMPLETE";
      }
      return "UNKNOWN_STATE";
    }

    void RikaStove::set_state(State1 state)
    {
      this->state1_ = state;
      ESP_LOGV(TAG, "State: %s", this->state_to_string_(this->state1_).c_str());
    }

    // void RikaStove::reset_stove_request()
    // {
    //   this->stove_request_ = "";
    // }

    // void RikaStove::reset_state()
    // {
    //   this->reset_stove_request();
    //   this->raw_stove_status_ = "";
    // }

    void RikaStove::send_crlf_()
    {
      this->write_byte(ASCII_CR);
      this->write_byte(ASCII_LF);
    }

    void RikaStove::send_ok_()
    {
      this->write_str("OK");
      this->send_crlf_();
      ESP_LOGD(TAG, "-> Response: OK");
    }

    void RikaStove::send_error_()
    {
      this->write_str("ERROR");
      this->send_crlf_();
      ESP_LOGD(TAG, "-> Response: ERROR");
    }

#ifdef USE_TEXT_SENSOR
    void RikaStove::set_sub_text_sensor(SubTextSensorType type, text_sensor::TextSensor *sens)
    {
      this->sub_text_sensors_[(size_t)type] = sens;
      switch (type)
      {
      case SubTextSensorType::STOVE_STATUS:
        sens->publish_state(this->status_message_);
        break;

      default:
        break;
      }
    }

    void RikaStove::update_sub_text_sensor_(SubTextSensorType type, const std::string &value)
    {
      size_t index = (size_t)type;
      if (this->sub_text_sensors_[index] != nullptr)
        this->sub_text_sensors_[index]->publish_state(value);
    }
#endif // USE_TEXT_SENSOR

  } // namespace rikastove
} // namespace esphome
