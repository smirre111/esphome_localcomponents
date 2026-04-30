#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esp_random.h"

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
      auto restore = this->restore_state_();
      if (restore.has_value())
      {
        restore->apply(this);
      }
      else {
        this->frame_counter_.rx_message_id = 0;
        this->frame_counter_.tx_message_id = 0;
        this->registered_ = false;

      }
      // this->encoder_ = make_unique<LoraCovEncoder>();
      // this->decoder_ = make_unique<LoraCovDecoder>();
    }

    optional<LORAClientRestoreState> LORAListener::restore_state_()
    {
      this->rtc_ = global_preferences->make_preference<LORAClientRestoreState>(this->get_preference_hash());
      LORAClientRestoreState recovered{};
      if (!this->rtc_.load(&recovered))
        return {};
      return recovered;
    }

    void LORAListener::save_state_(bool save)
    {
      if (save)
      {
        LORAClientRestoreState restore{};
        memset(&restore, 0, sizeof(restore));
        restore.rx_message_id = this->frame_counter_.rx_message_id;
        restore.tx_message_id = this->frame_counter_.tx_message_id;
        restore.logged_in = this->registered_;

        this->rtc_.save(&restore);
      }
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


      //Registering a new device is allowed in all cases
      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
      {
        ClientRegister *reg = rcv_message->register_;
        ESP_LOGI(TAG, "Received register command for MAC address: %d", reg->mac_addr);
        ESP_LOGI(TAG, "This clients' MAC address: %d", this->address_uint64_);
        if (reg->mac_addr != this->address_uint64_)
        {
          ESP_LOGE(TAG, "%s, MAC address does not match, ignoring register command", this->get_name().c_str());
          return;
        }

        ESP_LOGI(TAG, "%s, Registered with LORA server", this->get_name().c_str());
        this->registered_ = true;
        this->send_remote_config();
        return;
      }





      ESP_LOGI(TAG, "Received message with ID %d from address %d", rcv_message->msgid, rcv_message->senderaddress);
      ESP_LOGI(TAG, "My message ID: %d", this->frame_counter_.rx_message_id);
      ESP_LOGI(TAG, "My address: %d",  this->short_address_);

      if (this->registered_ == false)
      {
        ESP_LOGE(TAG, "%s, Not registered yet, ignoring message", this->get_name().c_str());
        return;
      }

      if ((rcv_message->senderaddress != this->short_address_))
      {
        ESP_LOGE(TAG, "%s, Address not for me", this->get_name().c_str());
        return;
      }

      if (rcv_message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_LOGIN)
      {
        // We ignore the prand form the client and send him a new one, to avoid replay attacks. 
        //The client has to use the prand we send him in the login response for all further communication, so it will be forced to use the new prand.
        // LoginMsg *login = rcv_message->login;
        // ESP_LOGI(TAG, "%s, Received login command with prand: %d", this->get_name().c_str(), login->prand);


        // this->frame_counter_.tx_message_id = login->prand;
        // this->frame_counter_.rx_message_id = login->prand;

        this->send_login();
        return;
      }


      // The message ID is checked only after login
      if (rcv_message->proto_case != LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER)
      {
        if (rcv_message->msgid > this->frame_counter_.rx_message_id)
        {
          this->setRxMessageId(rcv_message->msgid);
          // this->frame_counter_.rx_message_id = rcv_message->msgid;
        }
        else
        {
          ESP_LOGE(TAG, "%s, duplicate or old message ID: %d, ignoring. My MsgID: %d",this->get_name().c_str(), rcv_message->msgid, this->frame_counter_.rx_message_id);
          return;
        }
      }


      // Distriute to registered nodes
      for (int i = 0; i < this->nodes_.size(); i++)
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

      op_message.msgid = this->incrTxMessageId(); // Incrementing message ID

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

      op_message.msgid = this->incrTxMessageId(); // Incrementing message ID

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


    uint32_t LORAListener::incrTxMessageId()
    {
      return ++this->frame_counter_.tx_message_id;
    }

    void LORAListener::setRxMessageId(uint32_t msg_id)
    {
      this->frame_counter_.rx_message_id = msg_id;
    }

    void LORAListener::send_login()
    {
      ESP_LOGI(TAG, "Sending login config for device %s", this->address_str());
      // Implementation to send remote address
      // Generate random prand for login challenge
      uint32_t prand = esp_random() & 0x0000FFFF; //esp_random(void)
      this->frame_counter_.tx_message_id = 0;
      this->frame_counter_.rx_message_id = 0;

      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address

      op_message.destaddress = this->short_address_;
      op_message.destsubnet = this->subnet_address_;
      op_message.senderaddress = 0xFF; // TODO: Use unique address

      op_message.msgid = this->incrTxMessageId(); //++(this->frame_counter_.tx_message_id); // Incrementing message ID
      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN;

      LoginMsg login = LOGIN_MSG__INIT; 
      login.prand = prand;


      ESP_LOGI(TAG, "Sending login challenge: %s", this->get_name().c_str());

      op_message.login = &login;

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

    void LORAListener::send_remote_config()
    {
      ESP_LOGI(TAG, "Sending remote config for device %s", this->address_str());
      // Implementation to send remote address

      this->frame_counter_.tx_message_id = 0;
      this->frame_counter_.rx_message_id = 0;

      LoraClientOperationMessage op_message LORA_CLIENT_OPERATION_MESSAGE__INIT;
      // op_message.destaddress = esphome::lora_tracker::LORATracker::broadcastAddressing;
      // op_message.destsubnet = esphome::lora_tracker::LORATracker::subnetAddressing;
      // op_message.senderaddress = 0x12345678; // TODO: Use unique address

      op_message.destaddress = this->short_address_;
      op_message.destsubnet = this->subnet_address_;
      op_message.senderaddress = 0xFF; // TODO: Use unique address

      op_message.msgid = this->incrTxMessageId(); //++(this->frame_counter_.tx_message_id); // Incrementing message ID
      op_message.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG;

      ClientConfig clientconfig = CLIENT_CONFIG__INIT;
      clientconfig.mac_addr = this->address_uint64_;
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
