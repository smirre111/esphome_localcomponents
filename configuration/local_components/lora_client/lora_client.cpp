#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"

namespace esphome
{
  namespace lora_tracker
  {
    static const char *const TAG = "lora_client";

    void LORAListener::dump_config()
    {

      ESP_LOGCONFIG(TAG,
                    "  Short address: %d\n"
                    "  Subnet address: %d\n"
                    "  Sleep duration: %d",
                    this->short_address_, this->subnet_address_, this->sleep_duration_);
    }

    void LORAListener::setup()
    {
      // this->position = COVER_OPEN;
      // this->encoder_ = make_unique<LoraCovEncoder>();
      // this->decoder_ = make_unique<LoraCovDecoder>();
      this->logged_in_ = false;
    }

    // void LORAListener::send_remote_address()
    // {
    //   ESP_LOGI("LORAListener", "Sending remote address for device %s", this->address_str());
    //   // Implementation to send remote address

    //   BlndOperationMessage op_message BLND_OPERATION_MESSAGE__INIT;
    //   op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
    //   op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
    //   op_message.senderaddress = 0x12345678; // TODO: Use unique address
    //   op_message.msgid = 0;                  // Incrementing message ID
    //   op_message.cmd_case = BLND_OPERATION_MESSAGE__CMD_ADDR;

    //   AddressUpdate addr_update = ADDRESS_UPDATE__INIT;
    //   addr_update.addr = this->short_address_;
    //   addr_update.subnt = this->subnet_address_;
    //   op_message.addr = &addr_update;

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
    //     ESP_LOGW(TAG, "Error writing address command to device");
    //   }

    // }

    // void LORAListener::send_remote_sleep_time()
    // {
    //   ESP_LOGI("LORAListener", "Setting sleep time for device %s", this->address_str());
    //   // Implementation to set sleep time

    //  BlndOperationMessage op_message BLND_OPERATION_MESSAGE__INIT;
    //   op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
    //   op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
    //   op_message.senderaddress = 0x12345678; // TODO: Use unique address
    //   op_message.msgid = 0;                  // Incrementing message ID
    //   op_message.cmd_case = BLND_OPERATION_MESSAGE__CMD_SLEEP;

    //   SleepUpdate sleep_update = SLEEP_UPDATE__INIT;
    //   sleep_update.sleepduration = this->sleep_duration_; // Example sleep time
    //   op_message.sleep = &sleep_update;

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
    //     ESP_LOGW(TAG, "Error writing sleep timecommand to device");

    //   }

    // }

void LORAListener::set_response(uint8_t *data, size_t len)
    {
      LoraClientResponseMessage *rcv_message;

      rcv_message = lora_client_response_message__unpack(NULL, len, data);

      if (rcv_message == NULL)
      {
        ESP_LOGE(TAG, "Could not read protobuf");
        return;
      }



      // The message ID is checked only after login
      if (rcv_message->proto_case != LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER && this->logged_in_)
      {
        if (rcv_message->msgid > this->rx_message_id_)
        {
          this->rx_message_id_ = rcv_message->msgid;
        }
        else
        {
          ESP_LOGE(TAG, "Duplicate or old message ID: %d, ignoring. My MsgID: %d", rcv_message->msgid, this->rx_message_id_);
          return;
        }
      }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
      {
        ESP_LOGI(TAG, "Registered with LORA server");
        this->logged_in_ = true;
        this->send_remote_config();

      }

      if (rcv_message->senderaddress != this->short_address_ && this->logged_in_)
      {
        ESP_LOGE(TAG, "Adress not for me");
        return;
      }
      
      //Distriute to registered nodes
      for(int i=0; i < this->nodes_.size(); i++)
      {
        this->nodes_[i]->set_response(data, len);
      }
    }



    void LORAListener::enterSleep()
    {
      ESP_LOGI("LORAListener", "Device %s entering sleep mode", this->address_str());
      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address
      op_message.destaddress = this->short_address_;
      op_message.destsubnet = this->subnet_address_;
      op_message.senderaddress = 0xFF; // TODO: Use unique address

      op_message.msgid = ++(this->tx_message_id_); // Incrementing message ID

      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
      op_message.sysop = CLIENT_OPERATION__CMD_SLEEP;
      auto status = false;

      uint8_t *txBuf;
      unsigned len;
      len = lora_client_operation_message__get_packed_size(&op_message);
      txBuf = new uint8_t[len];
      lora_client_operation_message__pack(&op_message, txBuf);

      // this->parent_->sendPacketOnce(txBuf, len);
      this->parent_->send(txBuf, len);

      if (status)
      {
        ESP_LOGW(TAG, "Error writing sleep command to device");
      }
    }


    void LORAListener::triggerOTA()
    {
      ESP_LOGI("LORAListener", "Device %s triggering OTA", this->address_str());
      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address
      op_message.destaddress = this->short_address_;
      op_message.destsubnet = this->subnet_address_;
      op_message.senderaddress = 0xFF; // TODO: Use unique address

      op_message.msgid = ++(this->tx_message_id_); // Incrementing message ID

      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
      op_message.sysop = CLIENT_OPERATION__CMD_OTA;
      auto status = false;

      uint8_t *txBuf;
      unsigned len;
      len = lora_client_operation_message__get_packed_size(&op_message);
      txBuf = new uint8_t[len];
      lora_client_operation_message__pack(&op_message, txBuf);

      // this->parent_->sendPacketOnce(txBuf, len);
      this->parent_->send(txBuf, len);

      if (status)
      {
        ESP_LOGW(TAG, "Error writing sleep command to device");
      }
    }



    void LORAListener::send_remote_config()
    {
      ESP_LOGI(TAG, "Sending remote config for device %s", this->address_str());
      // Implementation to send remote address

      this->tx_message_id_ = 0;
      this->rx_message_id_ = 0;

      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address

      op_message.destaddress = this->short_address_;
      op_message.destsubnet = this->subnet_address_;
      op_message.senderaddress = 0xFF; // TODO: Use unique address

      op_message.msgid = ++(this->tx_message_id_); // Incrementing message ID
      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG;

      ClientConfig clientconfig = CLIENT_CONFIG__INIT;
      clientconfig.addr = this->short_address_;
      clientconfig.subnt = this->subnet_address_;
      clientconfig.name.data = (uint8_t *)this->get_name().c_str();
      clientconfig.name.len = strlen(this->get_name().c_str());
      clientconfig.sleepduration = this->sleep_duration_;

      // TimeUpdate time_update = TIME_UPDATE__INIT;
      // time_update.closetime = this->close_duration_;
      // time_update.opentime = this->open_duration_;



      ESP_LOGI(TAG, "Setting device name to: %s", this->get_name().c_str());

      op_message.clientconfig = &clientconfig;

      auto status = false;

      uint8_t *txBuf;
      unsigned len;
      len = lora_client_operation_message__get_packed_size(&op_message);
      txBuf = new uint8_t[len];
      lora_client_operation_message__pack(&op_message, txBuf);

      // this->parent_->sendPacketOnce(txBuf, len);
      this->parent_->send(txBuf, len);
      delete[] txBuf;
      if (status)
      {
        ESP_LOGW(TAG, "Error writing address command to device");
      }

    }





  } // namespace lora_tracker
} // namespace esphome
