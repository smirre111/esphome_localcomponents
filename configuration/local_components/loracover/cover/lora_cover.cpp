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
        // F-4: tracked send — retransmits until the node returns a MAC ACK.
        // A node the hub models as asleep IGNORES the command (user decision):
        // the entity is left exactly as it was and the unchanged state is
        // re-published so Home Assistant drops whatever the UI showed optimistically.
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

          // F-4: tracked send — the operation is retransmitted with a fresh
          // msgid until acknowledged, then surfaced as failed if never acked.
          bool accepted;
          if (pos == COVER_OPEN)
          {
            accepted = this->parent_->send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                                                           COV_OPERATION__CMD_OPEN, 0.0f);
          }
          else if (pos == COVER_CLOSED)
          {
            accepted = this->parent_->send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                                                           COV_OPERATION__CMD_CLOSE, 0.0f);
          }
          else
          {
            accepted = this->parent_->send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, pos);
          }

          if (!accepted)
          {
            // Ignored: the node sleeps. Nothing moves, so the entity stays at its
            // real position, is not "busy", and the unchanged state is re-published
            // (without persisting) so the UI snaps back instead of showing the
            // requested position.
            this->publish_state(false);
            return;
          }
          this->target_position_ = pos;
          busy_ = true;
          const uint32_t now = millis();
          this->start_dir_time_ = now;
        }
      }
    }

    // S6: the node REGISTERed -- push this cover's configuration. Was the
    // REGISTER branch of set_response(), ahead of its own address filter.
    void LoraCoverComponent::on_register()
    {
      ESP_LOGI(TAG, "Received REGISTER — sending CoverConfig");
      this->send_remote_config();
    }

    // S6: a decoded application uplink from THIS cover's node, already
    // demultiplexed, address-filtered and (with a session) authenticated by the
    // listener. The cover no longer unpacks wire bytes or runs its own sender
    // filter (whose `registered_ == false` case let every sender through).
    void LoraCoverComponent::on_app_indication(const esphome::lora_tracker::AppIndication &ind)
    {
      if (ind.msg == nullptr ||
          ind.msg->proto_case != hubframes::UL_POSITION ||
          ind.msg->position == nullptr)
        return;
      this->position = ind.msg->position->position;
      this->publish_state(true);
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

      CoverConfig coverconfig = COVER_CONFIG__INIT;
      coverconfig.closetime = this->close_duration_;
      coverconfig.opentime = this->open_duration_;
      coverconfig.blindheightmm = this->blind_height_mm_;
      coverconfig.axlediametermm = this->axle_diameter_mm_;
      coverconfig.blindthicknessmm = this->blind_thickness_mm_;
      coverconfig.openslack = this->open_slack_;
      coverconfig.closeslack = this->close_slack_;

      // Through the listener's delivery mechanism, so it is ENCRYPTED once a session
      // is confirmed (header and msgid are its job too). This packed raw and called
      // parent_->parent_->send(), which put the CoverConfig in the clear on a
      // confirmed session, where the node refuses it: its geometry and durations
      // never reached an already-provisioned node.
      this->parent_->send_cover_config(coverconfig);
    }

  } // namespace LoraCov
} // namespace esphome
