#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/lora_client/lora_client_node.h"

#include "esphome/components/homeassistant/time/homeassistant_time.h"

#include <esp_bt_defs.h> //For esp_bd_addr_t

#include <array>
#include <string>
#include <vector>

namespace esphome
{
  namespace lora_tracker
  {

    class LORATracker;
    class LORAClientNode;

    class LORAListener : public EntityBase, public Component
    {
    public:
      static constexpr size_t MAC_ADDR_STR_LEN = 18; // "AA:BB:CC:DD:EE:FF\0"

      void setup() override;
      void dump_config() override;

      void set_parent(LORATracker *parent) { parent_ = parent; }
      void set_short_address(uint8_t address) { this->short_address_ = address; }
      void set_subnet_address(uint8_t address) { this->subnet_address_ = address; }
      void set_sleep_duration(uint16_t sleep_duration) { this->sleep_duration_ = sleep_duration; }
      void enterSleep();
      void triggerOTA();
      virtual void set_response(uint8_t *data, size_t len);

      // void send_remote_address();
      // void send_remote_sleep_time();
      virtual void send_remote_config();

      // std::string address_str() const;
      uint64_t address_uint64() const { return this->address_uint64_; };

      const uint8_t *address() const { return (const uint8_t *)(&this->address_); }
      uint8_t *get_remote_bda() { return this->remote_bda_; }
      uint64_t get_address() const { return this->address_uint64_; }
      virtual void set_address(uint64_t address)
      {
        this->address_uint64_ = address;
        this->remote_bda_[5] = (address >> 40) & 0xFF;
        this->remote_bda_[4] = (address >> 32) & 0xFF;
        this->remote_bda_[3] = (address >> 24) & 0xFF;
        this->remote_bda_[2] = (address >> 16) & 0xFF;
        this->remote_bda_[1] = (address >> 8) & 0xFF;
        this->remote_bda_[0] = (address >> 0) & 0xFF;

        this->address_[5] = (address >> 40) & 0xFF;
        this->address_[4] = (address >> 32) & 0xFF;
        this->address_[3] = (address >> 24) & 0xFF;
        this->address_[2] = (address >> 16) & 0xFF;
        this->address_[1] = (address >> 8) & 0xFF;
        this->address_[0] = (address >> 0) & 0xFF;

        if (address == 0)
        {
          this->address_str_[0] = '\0';
        }
        else
        {
          format_mac_addr_upper(this->remote_bda_, this->address_str_);
        }
      }
      const char *address_str() const { return this->address_str_; }
      bool check_addr(esp_bd_addr_t &addr) { return memcmp(addr, this->remote_bda_, sizeof(esp_bd_addr_t)) == 0; }

      void set_time(time::RealTimeClock *time) { this->time = time; }

      void register_lora_node(LORAClientNode *node)
      {
        // node->client = this;
        node->set_lora_client_parent(this);
        this->nodes_.push_back(node);
      }

    protected:
      std::vector<LORAClientNode *> nodes_;

    public:
      LORATracker *parent_{nullptr};
    
    protected:
      time::RealTimeClock *time;

      // Group 1: 8-byte types
      uint64_t address_uint64_{0};
      esp_bd_addr_t address_{
          0,
      };

    public:
      uint8_t short_address_{0};
      uint8_t subnet_address_{0};
      uint64_t sleep_duration_{86400}; // Default to 24 hours
      bool logged_in_;

      uint32_t rx_message_id_{0};
      uint32_t tx_message_id_{0};



      char address_str_[MAC_ADDR_STR_LEN]{}; // 18 bytes: "AA:BB:CC:DD:EE:FF\0"
     
    protected: 
      esp_bd_addr_t remote_bda_;             // 6 bytes


    };

    class LORAClient : public LORAListener
    {
    public:
      // Memory optimized layout
      uint8_t app_id; // App IDs are small integers assigned sequentially

    protected:
    };

  }
}