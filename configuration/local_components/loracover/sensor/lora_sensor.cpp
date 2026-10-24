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

    // A reading outside this window is not a battery state.
    //
    // A node that has not completed a measurement yet — fresh boot, or a
    // deep-sleep wake before its first ADC cycle — reports 0 V out of its
    // last-known-good cache, and both the STATE and POSITION frames carry that
    // cached value. Published, it lands in Home Assistant's history as a 0 V /
    // 0 % point that no battery ever had, and it is the long-run history these
    // sensors exist for.
    //
    // DELIBERATELY WIDER than the 9.6-12.6 V the percentage maps: this rejects
    // "no measurement" and corruption, not a flat or overcharged pack, which
    // are real states an operator needs to see. They must bracket the pack
    // range with room to spare, hence the static_asserts.
    static constexpr float kPlausibleMinV = 6.0f;
    static constexpr float kPlausibleMaxV = 15.0f;
    static_assert(kPlausibleMinV < kCellEmptyV * kCellCount,
                  "the plausibility floor must sit below an empty pack, or a "
                  "genuinely flat battery would be discarded as noise");
    static_assert(kPlausibleMaxV > kCellFullV * kCellCount,
                  "and the ceiling above a full one");

    void LoraCover::publish_battery_(float voltage)
    {
      if (voltage < kPlausibleMinV || voltage > kPlausibleMaxV)
      {
        ESP_LOGW(TAG, "Ignoring implausible battery voltage %.2f V", voltage);
        return;
      }

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

    // S6: the node REGISTERed. The sensor platform has no configuration of its
    // own to push (send_remote_config is a no-op), as before.
    void LoraCover::on_register()
    {
      ESP_LOGI(TAG, "Received REGISTER — sending sensor config (no-op)");
      this->send_remote_config();
    }

    // S6: a decoded application uplink from THIS sensor's node (see
    // AppIndication): no wire unpack, no own address filter.
    void LoraCover::on_app_indication(const esphome::lora_tracker::AppIndication &ind)
    {
      const hubframes::Up *m = ind.msg;
      if (m == nullptr)
        return;

      if (m->proto_case == hubframes::UL_STATE && m->state != nullptr)
      {
        this->publish_battery_(m->state->voltage);
        this->publish_link_rssi_();
      }

      if (m->proto_case == hubframes::UL_POSITION && m->position != nullptr)
      {
        const CoverPosition *position = m->position;
        this->publish_battery_(position->voltage);
        // F-11: motor current rides in the position frame (raw ADC counts).
        if (this->motor_current_ != nullptr)
          this->motor_current_->publish_state(position->current);
        // Battery-runtime compensation telemetry: 0/0 means "no calibration
        // event in this packet" (proto3 "absent") -- skip the publish rather
        // than show a misleading 0 s point in the history graph; the sensor
        // just holds its last real value until the next one.
        if (this->calib_actual_run_ != nullptr && position->actualruns > 0.0f)
          this->calib_actual_run_->publish_state(position->actualruns);
        if (this->calib_nominal_run_ != nullptr && position->nominalruns > 0.0f)
          this->calib_nominal_run_->publish_state(position->nominalruns);
        this->publish_link_rssi_();
      }
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
