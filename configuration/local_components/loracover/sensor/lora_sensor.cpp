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


    // void LoraCov::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
    // {
    //   switch (event)
    //   {
    //   case ESP_GATTC_OPEN_EVT:
    //   {
    //     if (param->open.status == ESP_GATT_OK)
    //     {
    //       this->registered_ = false;
    //     }
    //     break;
    //   }
    //   case ESP_GATTC_DISCONNECT_EVT:
    //   {
    //     this->registered_ = false;
    //     this->node_state = espbt::ClientState::IDLE;
    //     if (this->battery_ != nullptr)
    //       this->battery_->publish_state(NAN);
    //     if (this->illuminance_ != nullptr)
    //       this->illuminance_->publish_state(NAN);
    //     break;
    //   }
    //   case ESP_GATTC_SEARCH_CMPL_EVT:
    //   {
    //     auto *chr = this->parent_->get_characteristic(LORA_COVER_SERVICE_UUID, LORA_COVER_CHARACTERISTIC_UUID);
    //     if (chr == nullptr)
    //     {
    //       if (this->parent_->get_characteristic(LORA_COVER_TUYA_SERVICE_UUID, LORA_COVER_TUYA_CHARACTERISTIC_UUID) != nullptr)
    //       {
    //         ESP_LOGE(TAG, "[%s] Detected a Tuya LORA_COVER which is not supported, sorry.",
    //                  this->parent_->address_str().c_str());
    //       }
    //       else
    //       {
    //         ESP_LOGE(TAG, "[%s] No control service found at device, not an LORA_COVER..?",
    //                  this->parent_->address_str().c_str());
    //       }
    //       break;
    //     }
    //     this->char_handle_ = chr->handle;
    //     break;
    //   }
    //   case ESP_GATTC_REG_FOR_NOTIFY_EVT:
    //   {
    //     this->node_state = espbt::ClientState::ESTABLISHED;
    //     this->update();
    //     break;
    //   }
    //   case ESP_GATTC_NOTIFY_EVT:
    //   {
    //     if (param->notify.handle != this->char_handle_)
    //       break;
    //     this->decoder_->decode(param->notify.value, param->notify.value_len);

    //     if (this->battery_ != nullptr && this->decoder_->has_battery_level() &&
    //         millis() - this->last_battery_update_ > 10000)
    //     {
    //       this->battery_->publish_state(this->decoder_->battery_level_);
    //       this->last_battery_update_ = millis();
    //     }

    //     if (this->illuminance_ != nullptr && this->decoder_->has_light_level())
    //     {
    //       this->illuminance_->publish_state(this->decoder_->light_level_);
    //     }

    //     if (this->current_sensor_ > 0)
    //     {
    //       if (this->illuminance_ != nullptr)
    //       {
    //         auto *packet = this->encoder_->get_light_level_request();
    //         auto status = esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(),
    //                                                this->char_handle_, packet->length, packet->data,
    //                                                ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    //         if (status)
    //         {
    //           ESP_LOGW(TAG, "[%s] esp_ble_gattc_write_char failed, status=%d", this->parent_->address_str().c_str(),
    //                    status);
    //         }
    //       }
    //       this->current_sensor_ = 0;
    //     }
    //     break;
    //   }
    //   default:
    //     break;
    //   }
    // }


    void LoraCover::set_response(uint8_t *data, size_t len)
    {
      LoraClientResponseMessage *rcv_message;

      rcv_message = lora_client_response_message__unpack(NULL, len, data);

      if (rcv_message == NULL)
      {
        ESP_LOGE(TAG, "Could not read protobuf");
        return;
      }

      // if (rcv_message->senderaddress != this->parent_->short_address_ && this->parent_->registered_)
      // {
      //   ESP_LOGE(TAG, "Adress not for me");
      //   return;
      // }

      // // The message ID is checked only after login
      // if (rcv_message->proto_case != LORA_CLIENT_RESPONSE_MESSAGE__PROTO_AVAIL && this->parent_->registered_)
      // {
      //   if (rcv_message->msgid > this->parent_->frame_counter_.rx_message_id)
      //   {
      //     this->parent_->rx_message_id_ = rcv_message->msgid;
      //   }
      //   else
      //   {
      //     ESP_LOGE(TAG, "Duplicate or old message ID: %d, ignoring. My MsgID: %d", rcv_message->msgid, this->parent_->rx_message_id_);
      //     return;
      //   }
      // }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
      {
        ESP_LOGI(TAG, "Registered with LORA server");

        this->send_remote_config();
        return;
      
      }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_STATE)
      {
        ClientBattery *status = rcv_message->state;

        float voltage = status->voltage;
        float battery_level = (voltage - 3.2*3) / (4.2*3 - 3.2*3) * 100.0;
        battery_level = std::clamp(battery_level, 0.0f, 100.0f);

        if (this->battery_ != nullptr)
        {
          this->battery_->publish_state(battery_level);

        }
        if (this->voltage_ != nullptr)
        {
          this->voltage_->publish_state(voltage);
        }
      }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION)
      {
        CoverPosition *position = rcv_message->position;

        float voltage = position->voltage;
        float battery_level = (voltage - 3.2*3) / (4.2*3 - 3.2*3) * 100.0;
        battery_level = std::clamp(battery_level, 0.0f, 100.0f);

        if (this->battery_ != nullptr)
        {
          this->battery_->publish_state(battery_level);

        }
        if (this->voltage_ != nullptr)
        {
          this->voltage_->publish_state(voltage);
        }
      }
     
    }



    void LoraCover::update()
    {

    }

  } // namespace am43
} // namespace esphome
