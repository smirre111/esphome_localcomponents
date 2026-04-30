#include "esphome/components/loracover/cover/lora_cover.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"

namespace esphome
{
  namespace loracov
  {

    static const char *const TAG = "lora_cover";

    using namespace esphome::cover;

    void LoraCoverComponent::dump_config()
    {
      LOG_COVER("", "Lora Cover", this);
      ESP_LOGCONFIG(TAG,
                    "  Device Pin: %d\n"
                    "  Invert Position: %d",
                    this->pin_, (int)this->invert_position_);
    }

    void LoraCoverComponent::setup()
    {

      auto restore = this->restore_state_();
      if (restore.has_value())
      {
        restore->apply(this);
      }

      if (!restore.has_value())
      {
        this->position = 0.5f;
      }

      // this->position = COVER_OPEN;
      this->encoder_ = make_unique<LoraCovEncoder>();
      this->decoder_ = make_unique<LoraCovDecoder>();
      // this->registered_ = false;
    }

    void LoraCoverComponent::loop()
    {

      // const uint32_t now = App.get_loop_component_start_time();

      // if ((now - this->start_dir_time_ > 2e3) && (this->busy_ == true))
      // {
      //   this->busy_ = false;
      //   ESP_LOGI(TAG, "'%s' - Assuming target position reached after 2s.", this->name_.c_str());
      //   if (this->position != this->target_position_)
      //   {
      //     this->position = this->target_position_;
      //     this->publish_state();
      //   }
      // }
    }

    CoverTraits LoraCoverComponent::get_traits()
    {
      auto traits = CoverTraits();
      traits.set_supports_stop(true);
      traits.set_supports_position(true);
      traits.set_supports_tilt(false);
      traits.set_is_assumed_state(true);
      return traits;
    }

    void LoraCoverComponent::control(const CoverCall &call)
    {

      if (call.get_stop())
      {
        auto *packet = this->encoder_->get_stop_request();
        auto status = false;
        // esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), this->char_handle_,
        //                          packet->length, packet->data, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);

        LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
        // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
        // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;

        op_message.destaddress = this->parent_->short_address_;
        op_message.destsubnet = this->parent_->subnet_address_;
        op_message.senderaddress = 0xFF; // TODO: Use unique address

        op_message.msgid = this->parent_->incrTxMessageId(); // Incrementing message ID

        op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION;
        
        LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
        covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
        covop.operation = COV_OPERATION__CMD_STOP;
        
        // op_message.operation = COV_OPERATION__CMD_STOP;
        op_message.operation = &covop;


        uint8_t *txBuf;
        unsigned len;
        len = lora_client_operation_message__get_packed_size(&op_message);
        txBuf = new uint8_t[len];
        lora_client_operation_message__pack(&op_message, txBuf);

        // this->parent_->sendPacketOnce(txBuf, len);
        this->parent_->parent_->send(txBuf, len);
        if (status)
        {
          ESP_LOGW(TAG, "[%s] Error writing stop command to device, error = %d", this->get_name().c_str(), status);
        }
        this->publish_state(true);
      }

      if (call.get_position().has_value())
      {
        auto pos = *call.get_position();
        if (pos == this->position)
        {
          // already at target
        }
        else
        {
          if (this->invert_position_)
            pos = 1 - pos;

          this->target_position_ = pos;
          auto *packet = this->encoder_->get_set_position_request(100 - (uint8_t)(pos * 100));

          LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
          // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
          // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
          // op_message.senderaddress = 0x12345678; // TODO: Use unique address
          op_message.destaddress = this->parent_->short_address_;
          op_message.destsubnet = this->parent_->subnet_address_;
          op_message.senderaddress = 0xFF; // TODO: Use unique address

          op_message.msgid = this->parent_->incrTxMessageId(); // Incrementing message ID
          op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION;
          LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
          if (pos == COVER_OPEN)
          {
            covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
            covop.operation = COV_OPERATION__CMD_OPEN;
          }
          else if (pos == COVER_CLOSED)
          {
            covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
            covop.operation = COV_OPERATION__CMD_CLOSE;
          }
          else
          {
            covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
            covop.position = pos;
          }
          op_message.operation = &covop;

          auto status = false;
          // esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), this->char_handle_,
          //                          packet->length, packet->data, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);

          uint8_t *txBuf;
          unsigned len;
          len = lora_client_operation_message__get_packed_size(&op_message);
          txBuf = new uint8_t[len];
          lora_client_operation_message__pack(&op_message, txBuf);
          // this->parent_->sendPacketOnce(txBuf, len);
          this->parent_->parent_->send(txBuf, len);

          if (status)
          {
            ESP_LOGW(TAG, "[%s] Error writing set_position command to device, error = %d", this->get_name().c_str(), status);
          }
          busy_ = true;
          const uint32_t now = millis();
          this->start_dir_time_ = now;
        }
      }
    }

    void LoraCoverComponent::set_response(uint8_t *data, size_t len)
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
      // if (rcv_message->proto_case != LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER && this->parent_->registered_)
      // {
      //   if (rcv_message->msgid > this->parent_->rx_message_id_)
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
        ESP_LOGI(TAG, "Registered with LORA server, sending cover config");

        this->send_remote_config();
        return;
      }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION)
      {
        CoverPosition *position = rcv_message->position;
        float pos = position->position;

        this->position = pos;
        this->publish_state(true);
      }
    }

    // void LoraCoverComponent::send_remote_duration()
    // {
    //   ESP_LOGI(TAG, "Setting remote close duration for device %s", this->address_str());
    //   // Implementation to set remote close duration

    //   BlndOperationMessage op_message BLND_OPERATION_MESSAGE__INIT;
    //   op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
    //   op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
    //   op_message.senderaddress = 0x12345678; // TODO: Use unique address
    //   op_message.msgid = 0;                  // Incrementing message ID
    //   op_message.cmd_case = BLND_OPERATION_MESSAGE__CMD_TIME;

    //   TimeUpdate time_update = TIME_UPDATE__INIT;
    //   time_update.closetime = this->close_duration_;
    //   time_update.opentime = this->open_duration_;
    //   op_message.time = &time_update;

    //   auto status = false;

    //   uint8_t *txBuf;
    //   unsigned len;
    //   len = blnd_operation_message__get_packed_size(&op_message);
    //   txBuf = new uint8_t[len];
    //   blnd_operation_message__pack(&op_message, txBuf);

    //   // this->parent_->sendPacketOnce(txBuf, len);
    //   this->parent_->send(txBuf, len);

    //   if (status)
    //   {
    //     ESP_LOGW(TAG, "Error writing sleep command to device");
    //   }

    // }

    void LoraCoverComponent::send_remote_config()
    {
      ESP_LOGI(TAG, "Sending remote config for device %s", this->parent_->address_str());
      // Implementation to send remote address

      // this->parent_->frame_counter_.tx_message_id = 0;
      // this->parent_->frame_counter_.rx_message_id = 0;

      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address

      op_message.destaddress = this->parent_->short_address_;
      op_message.destsubnet = this->parent_->subnet_address_;
      op_message.senderaddress = 0xFF; // TODO: Use unique address

      op_message.msgid = this->parent_->incrTxMessageId(); //++(this->parent_->frame_counter_.tx_message_id); Incrementing message ID
      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG;

      CoverConfig coverconfig = COVER_CONFIG__INIT;
      coverconfig.closetime = this->close_duration_;
      coverconfig.opentime = this->open_duration_;

      op_message.coverconfig = &coverconfig;

      auto status = false;

      uint8_t *txBuf;
      unsigned len;
      len = lora_client_operation_message__get_packed_size(&op_message);
      txBuf = new uint8_t[len];
      lora_client_operation_message__pack(&op_message, txBuf);

      // this->parent_->sendPacketOnce(txBuf, len);
      this->parent_->parent_->send(txBuf, len);
      delete[] txBuf;
      if (status)
      {
        ESP_LOGW(TAG, "Error writing address command to device");
      }
    }

  } // namespace LoraCov
} // namespace esphome
