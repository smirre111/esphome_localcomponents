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
  // Forward declaration so LORAListener can hold an optional "command failed"
  // binary sensor without forcing the binary_sensor component on every user of
  // this header (the full include lives in lora_client.cpp).
  namespace binary_sensor { class BinarySensor; }

  namespace lora_tracker
  {
    class LORATracker;
    class LORAClientNode;
    class LORAListener;

    struct FrameCounter
    {
      uint32_t rx_message_id;
      uint32_t tx_message_id;

    };

    struct LORAClientRestoreState
    {
      uint8_t  version;           // must equal LORAListener::kRestoreStateVersion; otherwise discarded
      uint32_t rx_message_id;
      uint32_t tx_message_id;
      bool     logged_in;
      uint32_t last_sleep_epoch;  // Unix timestamp when enterSleep() was last called; 0 = unknown

      void apply(LORAListener *listener);

    } __attribute__((packed));

 

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
      void send_login();
      void send_base_nonce_exchange();
      virtual void send_remote_config();
      uint32_t incrTxMessageId();
      void setRxMessageId(uint32_t msg_id);

      // F-4: Send a cover operation with delivery tracking.  The operation is
      // stored so it can be retransmitted (with a fresh, incrementing msgid)
      // until the node returns a CommandAck.  After kOpMaxRetries the command is
      // marked failed and surfaced to Home Assistant via the optional binary
      // sensor.  covop_case / operation use the generated LoraCoverOperation /
      // CovOperation enum values; position is used only for the POSITION case.
      void send_cover_operation(uint32_t covop_case, int32_t operation, float position);
      void set_command_failed_binary_sensor(binary_sensor::BinarySensor *bs) { this->command_failed_bsensor_ = bs; }

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
      uint8_t  short_address_{0};
      uint8_t  subnet_address_{0};
      uint64_t sleep_duration_{86400}; // Default to 24 hours
      bool     registered_;

      // uint32_t rx_message_id_{0};
      // uint32_t tx_message_id_{0};
      FrameCounter frame_counter_{0, 0};

      char address_str_[MAC_ADDR_STR_LEN]{}; // 18 bytes: "AA:BB:CC:DD:EE:FF\0"

      // Login state — cleared on every hub reboot or enterSleep(); set when the
      // node sends a valid message after the login challenge.
      uint32_t last_sleep_epoch_{0};   // Unix epoch when enterSleep() was last called
      bool     login_acked_{false};
      uint8_t  login_retry_count_{0};
      // Pending base-nonce for an in-flight login challenge.  Generated once
      // per challenge by send_login() and reused on every retry until the node
      // acknowledges; cleared when login_acked_ flips to true (or at sleep).
      // This prevents the hub from minting a fresh nonce on each retry — the
      // node rate-limits LoginMsg to one per 5 s and only stores the first
      // accepted one, so regenerating leaves hub and node with disagreeing
      // base nonces and every subsequent encrypted reply fails IV check.
      uint32_t pending_login_nonce_{0};
      // Set to true as soon as the startup-login path is initiated (either the
      // timer is armed or the login has already been sent).  Checked by the NTP
      // on_time_sync callback so that NTP resyncs (which fire the callback again)
      // do not disrupt an already-running session.  Unlike login_acked_, this flag
      // is never cleared by incoming messages.
      bool     startup_login_initiated_{false};
      // True once this hub boot has pushed configuration to the node.  Reset to
      // false on every hub reboot (member default), so a config change flashed
      // into the hub is delivered exactly once per node per boot; routine wakes
      // of an already-synced, provisioned node skip the config bursts entirely.
      bool     config_synced_{false};

      static constexpr uint8_t  kRestoreStateVersion  = 2;
      static constexpr uint8_t  kMaxLoginRetries      = 24; // 24 × 1 h = 1 day
      static constexpr uint32_t kLoginRetryIntervalMs = 3600000; // 1 h between login retries
      // Delay between receiving REGISTER and firing the login challenge.  The
      // REGISTER path first queues ClientConfig + each node's config, each sent
      // as a ~1.5-1.9 s burst.  A shorter delay (was 500 ms) queued LoginMsg on
      // top of those bursts, saturating the channel so the node's login-ack had
      // no quiet window.  4 s clears ~2 config bursts before login is sent.
      static constexpr uint32_t kRegisterToLoginDelayMs = 4000;
      // When config is skipped (provisioned node, already synced) there are no
      // config bursts to drain, so login can fire much sooner — shortening the
      // battery node's awake window.
      static constexpr uint32_t kRegisterToLoginFastMs  = 800;

    protected:
      esp_bd_addr_t remote_bda_; // 6 bytes

      optional<LORAClientRestoreState> restore_state_();
      void save_state_(bool save);

      // Login scheduling helpers
      bool     is_node_awake_() const;
      uint32_t ms_until_node_awake_() const;
      void     schedule_startup_login_();
      void     do_login_and_arm_retry_();
      // Arm a ONE-SHOT login retry that re-arms itself, mirroring
      // schedule_op_retry_().  Uses set_timeout rather than set_interval: an
      // interval's first firing is randomly phased within [0, interval), which
      // fired the "hourly" login retry within ~0.6 s of the challenge, stepping
      // on the node's burst-deferred login-ack and piling a redundant LoginMsg
      // burst on top of the register-triggered config bursts.
      void     schedule_login_retry_();

      // ---- F-4: command ACK / retransmit state ----
      bool     op_awaiting_ack_{false};
      uint32_t op_first_msgid_{0};   // first msgid used for the current logical command
      uint32_t op_last_msgid_{0};    // msgid of the most recent (re)transmission
      uint8_t  op_retry_count_{0};
      // Stored operation, replayed on each retransmission with a fresh msgid.
      uint32_t op_covop_case_{0};
      int32_t  op_operation_{0};
      float    op_position_{0.0f};
      bool     command_failed_{false};
      binary_sensor::BinarySensor *command_failed_bsensor_{nullptr};

      // 3000 ms (not 2000): a burst is ~1.5 s and the tracker holds a post-burst
      // RX window after it, so the cycle is ~1.9 s.  2000 ms re-burst almost
      // immediately, leaving no quiet gap for the node's deferred ACK — the
      // node's CAD saw the next burst as busy and backed off, so the ACK landed
      // a burst or two late.  3000 ms opens a ~1 s quiet listening gap where the
      // deferred ACK arrives and cancels the retransmit before it fires.
      static constexpr uint32_t kOpRetryIntervalMs = 3000;
      static constexpr uint8_t  kOpMaxRetries      = 4;

      uint32_t tx_cover_operation_();        // (re)pack + send the stored op, returns msgid
      void     handle_command_ack_(uint32_t ack_msg_id);
      void     set_command_failed_(bool failed);
      // Arm a ONE-SHOT retransmit timer.  Uses set_timeout (deterministic
      // now+delay) rather than set_interval, whose first firing is randomly
      // offset within [0, interval/2) to de-correlate periodic components — that
      // jitter fired the first cover-op retransmit almost immediately.  The
      // timeout re-arms itself after each retransmit; cancelled on ack.
      void     schedule_op_retry_();

      ESPPreferenceObject rtc_;
    };

    inline void LORAClientRestoreState::apply(LORAListener *listener)
    {
      listener->frame_counter_.rx_message_id = this->rx_message_id;
      listener->frame_counter_.tx_message_id = this->tx_message_id + 64;
      listener->registered_       = this->logged_in;
      listener->last_sleep_epoch_ = this->last_sleep_epoch;
    }

    class LORAClient : public LORAListener
    {
    public:
      // Memory optimized layout
      uint8_t app_id; // App IDs are small integers assigned sequentially

    protected:
    };

  }
}