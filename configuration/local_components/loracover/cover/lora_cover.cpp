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
        // F-4: tracked send — retransmits until the node returns a CommandAck.
        this->parent_->send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                                            COV_OPERATION__CMD_STOP, 0.0f);
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

          // F-4: tracked send — the operation is retransmitted with a fresh
          // msgid until acknowledged, then surfaced as failed if never acked.
          if (pos == COVER_OPEN)
          {
            this->parent_->send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                                                COV_OPERATION__CMD_OPEN, 0.0f);
          }
          else if (pos == COVER_CLOSED)
          {
            this->parent_->send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                                                COV_OPERATION__CMD_CLOSE, 0.0f);
          }
          else
          {
            this->parent_->send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, pos);
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
        ESP_LOGI(TAG, "Received REGISTER — sending CoverConfig");
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

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION)
      {
        CoverPosition *position = rcv_message->position;
        float pos = position->position;

        this->position = pos;
        this->publish_state(true);
      }
      lora_client_response_message__free_unpacked(rcv_message, NULL);
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
      LoraHeader header = LORA_HEADER__INIT;
      header.destaddress = this->parent_->short_address_;
      header.destsubnet = this->parent_->subnet_address_;
      header.senderaddress = lora_tracker::kHubAddress;
      header.msgid = this->parent_->incrTxMessageId(); //++(this->parent_->frame_counter_.tx_message_id); Incrementing message ID
      op_message.header = &header;
      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG;

      CoverConfig coverconfig = COVER_CONFIG__INIT;
      coverconfig.closetime = this->close_duration_;
      coverconfig.opentime = this->open_duration_;
      coverconfig.blindheightmm = this->blind_height_mm_;
      coverconfig.axlediametermm = this->axle_diameter_mm_;
      coverconfig.blindthicknessmm = this->blind_thickness_mm_;

      op_message.coverconfig = &coverconfig;

      uint8_t *txBuf;
      unsigned len;
      len = lora_client_operation_message__get_packed_size(&op_message);
      txBuf = new uint8_t[len];
      lora_client_operation_message__pack(&op_message, txBuf);

      // this->parent_->sendPacketOnce(txBuf, len);
      this->parent_->parent_->send(txBuf, len);
      delete[] txBuf;
    }

  } // namespace LoraCov
} // namespace esphome
