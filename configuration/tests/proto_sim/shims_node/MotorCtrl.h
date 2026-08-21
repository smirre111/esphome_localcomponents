// Stub MotorCtrl — captures the calls CmdDispatcher.cpp makes so tests
// can inspect "what would the motor have done?" without any GPIO/PWM/ADC.
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <common.h>

#include <string>

struct State {
    float position_{0.0f};
};

class MotorCtrl {
public:
    MotorCtrl() {
        motorCmdQueueNew  = xQueueCreate(8, sizeof(MotorCmd_t));
        motorBatteryQueue = xQueueCreate(4, sizeof(uint8_t));
        motCmdQueueNew    = xQueueCreate(8, sizeof(MotorCmd_t));
    }

    void  setTargetPosition(float p)                { target_position_ = p; }
    float getPosition() const                       { return state_.position_; }
    State getState() const                          { return state_; }
    int   getLastMotorCurrentAdcRaw() const         { return 0; }
    void  restorePosition()                         {}
    void  setRollGeometry(float axle, float thick, float height) {
        axle_  = axle;
        thick_ = thick;
        h_     = height;
        geometry_set_ = true;
    }
    // Bar-TRAVEL-only durations, plus the slat-slack head/tail times applied
    // at the sill (v1.0.10).
    void  setRuntime(uint32_t open_s, uint32_t close_s) {
        open_s_ = open_s; close_s_ = close_s;
    }
    void  setSlack(uint32_t open_slack_s, uint32_t close_slack_s) {
        open_slack_s_ = open_slack_s; close_slack_s_ = close_slack_s;
    }

    QueueHandle_t motorCmdQueueNew;
    QueueHandle_t motorBatteryQueue;
    QueueHandle_t motCmdQueueNew;

    // Test inspection.
    float target_position() const { return target_position_; }
    bool  geometry_set()    const { return geometry_set_; }
    float axle_mm()         const { return axle_; }
    float thickness_mm()    const { return thick_; }
    float height_mm()       const { return h_; }
    uint32_t open_time_s()   const { return open_s_; }
    uint32_t close_time_s()  const { return close_s_; }
    uint32_t open_slack_s()  const { return open_slack_s_; }
    uint32_t close_slack_s() const { return close_slack_s_; }

private:
    State state_;
    float target_position_{0.0f};
    bool  geometry_set_{false};
    float axle_{0.0f}, thick_{0.0f}, h_{0.0f};
    uint32_t open_s_{0}, close_s_{0};
    uint32_t open_slack_s_{0}, close_slack_s_{0};
};
