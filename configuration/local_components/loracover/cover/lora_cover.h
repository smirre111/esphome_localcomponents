#pragma once

#include "esphome/core/component.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/loracover/loracover_base.h"




namespace esphome
{
  namespace loracov
  {

    namespace esplora = esphome::lora_tracker;

    class LoraCoverComponent : public cover::Cover, public esphome::lora_tracker::LORAClientNode, public Component
    {
    public:
      void setup() override;
      void loop() override;

      void dump_config() override;
      cover::CoverTraits get_traits() override;
      
      void set_pin(uint16_t pin) { this->pin_ = pin; }
      void set_invert_position(bool invert_position) { this->invert_position_ = invert_position; }
      void set_open_duration(int open_duration) { this->open_duration_ = open_duration; }
      void set_close_duration(int close_duration) { this->close_duration_ = close_duration; }
      // void set_sleep_duration(uint64_t sleep_duration) { this->sleep_duration_ = sleep_duration; }

      void set_response(uint8_t *data, size_t len) override;

      // void send_remote_duration();
      void send_remote_config() override;

    protected:
      void control(const cover::CoverCall &call) override;
      uint16_t char_handle_;
      
      uint16_t pin_;
      bool invert_position_;
      int open_duration_{60};
      int close_duration_{60};
      // uint64_t sleep_duration_{28800};

      std::unique_ptr<LoraCovEncoder> encoder_;
      std::unique_ptr<LoraCovDecoder> decoder_;
      float target_position_{0};

      uint32_t start_dir_time_{0};
      uint32_t last_publish_time_{0};
      bool busy_{false};

    };

  } // namespace loracov
} // namespace esphome
