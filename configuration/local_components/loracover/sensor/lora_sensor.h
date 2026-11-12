#pragma once

#include "esphome/core/component.h"

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome
{
  namespace loracov
  {

    class LoraCover : public esphome::lora_tracker::LORAClientNode, public PollingComponent
    {
    public:
      void setup() override;
      void update() override;
      // void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
      //                          esp_ble_gattc_cb_param_t *param) override;
      void on_app_indication(const esphome::lora_tracker::AppIndication &ind) override;
      void on_register() override;
      void publish_battery_(float voltage);
      void publish_link_rssi_();

      void dump_config() override;
      void set_battery(sensor::Sensor *battery) { battery_ = battery; }
      void set_voltage(sensor::Sensor *voltage) { voltage_ = voltage; }
      // F-11: link RSSI (hub-side) and motor current (from CoverPosition frame)
      void set_rssi(sensor::Sensor *rssi) { rssi_ = rssi; }
      void set_motor_current(sensor::Sensor *current) { motor_current_ = current; }
      // Battery-runtime compensation telemetry (visibility only -- the hub
      // never computes or feeds back a model; see
      // docs/battery-runtime-compensation-proposal.md decision 1). Raw pair
      // rather than a pre-divided ratio, matching the wire field's own
      // reasoning: more useful for later analysis, and proto3's "0 = absent"
      // convention means these only publish when a calibration event just
      // happened -- see on_app_indication()'s guard.
      void set_calib_actual_run(sensor::Sensor *s) { calib_actual_run_ = s; }
      void set_calib_nominal_run(sensor::Sensor *s) { calib_nominal_run_ = s; }
      // P2: node clock minus hub clock, from the wake beacon.
      void set_clock_offset(sensor::Sensor *offset) { clock_offset_ = offset; }
      void on_clock_offset(float offset_s) override;
      // The session diagnostics of this node (lora_client/SessionDiag.h): how
      // long the last session recovery took, and counters since hub boot.
      // Published on change only.
      void set_session_recovery_time(sensor::Sensor *s) { diag_[kDiagRecovery].sensor = s; }
      void set_login_messages(sensor::Sensor *s) { diag_[kDiagLogins].sensor = s; }
      void set_registers_accepted(sensor::Sensor *s) { diag_[kDiagRegAccepted].sensor = s; }
      void set_registers_refused(sensor::Sensor *s) { diag_[kDiagRegRefused].sensor = s; }
      void set_keyless_uplinks(sensor::Sensor *s) { diag_[kDiagKeyless].sensor = s; }
      void set_sessions_dropped(sensor::Sensor *s) { diag_[kDiagDropped].sensor = s; }
      void set_stale_periods(sensor::Sensor *s) { diag_[kDiagStale].sensor = s; }
      void on_session_diag() override;
      // void set_illuminance(sensor::Sensor *illuminance) { illuminance_ = illuminance; }

      void send_remote_config() override;
    protected:
      uint16_t char_handle_;
      // bool registered_;
      sensor::Sensor *battery_{nullptr};
      sensor::Sensor *voltage_{nullptr};
      sensor::Sensor *rssi_{nullptr};
      sensor::Sensor *motor_current_{nullptr};
      sensor::Sensor *calib_actual_run_{nullptr};
      sensor::Sensor *calib_nominal_run_{nullptr};
      sensor::Sensor *clock_offset_{nullptr};
      enum DiagSlot : uint8_t
      {
        kDiagRecovery, kDiagLogins, kDiagRegAccepted, kDiagRegRefused,
        kDiagKeyless, kDiagDropped, kDiagStale, kDiagCount
      };
      struct DiagSensor
      {
        sensor::Sensor *sensor{nullptr};
        bool            published{false};
        float           last{0.0f};
      };
      DiagSensor diag_[kDiagCount]{};
      void publish_diag_(DiagSlot slot, float value);
      // sensor::Sensor *illuminance_{nullptr};
      uint8_t current_sensor_;
      // The LoraCov often gets into a state where it spams loads of battery update
      // notifications. Here we will limit to no more than every 10s.
      uint8_t last_battery_update_;
    };

  } // namespace loracov
} // namespace esphome
