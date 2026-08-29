#include "lora_sensor.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome
{
  namespace loracov
  {

    static const char *const TAG = "loracov";

    void LoraCover::dump_config()
    {
      ESP_LOGCONFIG(TAG, "LORA_COVER");
      LOG_SENSOR(" ", "Battery", this->battery_);
    }

    void LoraCover::setup()
    {
      this->encoder_ = make_unique<LoraCovEncoder>();
      this->decoder_ = make_unique<LoraCovDecoder>();
      // this->registered_ = false;
      this->last_battery_update_ = 0;
      this->current_sensor_ = 0;
    }


 

    void LoraCover::send_remote_config()
    {

    }


    // The BLE GATT event handler this component was forked from lived here,
    // commented out — ~90 lines of esp_ble_gattc_* against a decoder/encoder
    // that this LoRa component never had. Removed; git has it if the original
    // Tuya cover is ever needed as a reference.


    // Battery percentage from pack voltage.
    //
    // Three cells in series, 3.2 V empty to 4.2 V full. STATE and POSITION
    // frames both carry a voltage and both published it, so this formula and
    // the two publish_state calls were written out twice — with the cell
    // figures inlined as 3.2*3 and 4.2*3 in both copies.
    static constexpr int   kCellCount  = 3;
    static constexpr float kCellEmptyV = 3.2f;
    static constexpr float kCellFullV  = 4.2f;

    void LoraCover::publish_battery_(float voltage)
    {
      const float empty_v = kCellEmptyV * kCellCount;
      const float full_v  = kCellFullV * kCellCount;
      float battery_level = (voltage - empty_v) / (full_v - empty_v) * 100.0f;
      battery_level = std::clamp(battery_level, 0.0f, 100.0f);

      if (this->battery_ != nullptr)
        this->battery_->publish_state(battery_level);
      if (this->voltage_ != nullptr)
        this->voltage_->publish_state(voltage);
    }

    // F-11: hub-side link RSSI for the packet just received.
    void LoraCover::publish_link_rssi_()
    {
      if (this->rssi_ != nullptr && this->parent_ != nullptr && this->parent_->parent_ != nullptr)
        this->rssi_->publish_state(this->parent_->parent_->get_last_rssi());
    }

    void LoraCover::set_response(uint8_t *data, size_t len)
    {
      LoraClientResponseMessage *rcv_message;

      rcv_message = lora_client_response_message__unpack(NULL, len, data);

      if (rcv_message == NULL)
      {
        ESP_LOGE(TAG, "Could not read protobuf");
        return;
      }

      if (!rcv_message->header)
      {
        ESP_LOGE(TAG, "Response missing header, ignoring");
        lora_client_response_message__free_unpacked(rcv_message, NULL);
        return;
      }

      // REGISTER is handled before the address filter: the node may not yet have
      // its assigned address in LittleFS (first-time or power-loss scenario).
      // LORAListener already verified the MAC before dispatching, so this is safe.
      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
      {
        ESP_LOGI(TAG, "Received REGISTER — sending sensor config (no-op)");
        this->send_remote_config();
        lora_client_response_message__free_unpacked(rcv_message, NULL);
        return;
      }

      if (rcv_message->header->senderaddress != this->parent_->short_address_ && this->parent_->registered_)
      {
        ESP_LOGE(TAG, "Adress not for me");
        lora_client_response_message__free_unpacked(rcv_message, NULL);
        return;
      }

      // NOTE: message-ID validation is done once by LORAListener::set_response
      // before dispatching bytes here.  Do NOT re-check it — the counter has
      // already been advanced and a second check would always reject the message.

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_STATE)
      {
        this->publish_battery_(rcv_message->state->voltage);
        this->publish_link_rssi_();
      }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION)
      {
        CoverPosition *position = rcv_message->position;
        this->publish_battery_(position->voltage);
        // F-11: motor current rides in the position frame (raw ADC counts).
        if (this->motor_current_ != nullptr)
          this->motor_current_->publish_state(position->current);
        this->publish_link_rssi_();
      }
      lora_client_response_message__free_unpacked(rcv_message, NULL);
    }



    void LoraCover::update()
    {

    }

    // P2: the node reported its own clock in a wake beacon. Publishing the
    // delta makes the crystal's real drift observable in Home Assistant rather
    // than only on a serial cable, which is what the uncapped-sleep decision
    // needs in order to stay honest over time.
    void LoraCover::on_clock_offset(float offset_s)
    {
      if (this->clock_offset_ != nullptr)
        this->clock_offset_->publish_state(offset_s);
    }

  } // namespace am43
} // namespace esphome
