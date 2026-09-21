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
    // Mirrors production: the restored position must also land in state_,
    // which is what the wake beacon reads.
    void  restorePosition()                         { state_.position_ = restore_to_; }
    void  set_restore_value(float p)                { restore_to_ = p; }
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

    // Mirrors the production accessor: true while a movement is in progress.
    // Tests drive it directly, since the harness has no FSM.
    bool isBusy() const { return busy_; }
    void set_busy(bool b) { busy_ = b; }

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

    // Motor current in AMPS, as taskMotorCurrentSensing converts it from the
    // VNH5019 CS reading. CmdDispatcher puts this in the CoverPosition frame
    // and the hub's sensor declares amps to match — the node-side conversion
    // and the hub-side unit only make sense together.
    //
    // The FSM still consumes RAW counts for its current-sense endstop, so both
    // accessors exist and they are not interchangeable.
    float getLastMotorCurrentAmps() const { return last_motor_current_amps_; }
    void  setLastMotorCurrentAmps(float a) { last_motor_current_amps_ = a; }

    // --- surface frtosTasks.cpp needs, as of T-1's last increment ---------
    //
    // The motor FSM and the battery supply switch are not protocol, so these
    // record rather than model. What compiling frtosTasks.cpp buys is the
    // INTERRUPT path (serviceDio0Event / serviceDio1Event); the motor tasks
    // come along because they are in the same translation unit, and their real
    // behaviour belongs on a bench next to HW-1.
    void fsmProcess(void * /*pvParameters*/) { fsm_calls_++; }
    unsigned fsmCalls() const { return fsm_calls_; }

    // The 12 V supply the battery divider hangs off. Production refcounts it
    // through these so a concurrent motor move cannot pull it out from under a
    // reading; the pair is recorded so a test can assert it BALANCES, which is
    // the only property that matters here.
    bool batteryAcquireSupply() { supply_acquires_++; return supply_ok_; }
    void batteryReleaseSupply() { supply_releases_++; }
    unsigned supplyAcquires() const { return supply_acquires_; }
    unsigned supplyReleases() const { return supply_releases_; }
    void     setSupplyAvailable(bool ok) { supply_ok_ = ok; }

    // Where taskMotorCurrentSensing publishes its averaged reading. Created
    // lazily so a test that never touches the motor path pays nothing.
    QueueHandle_t motorCurrentAdcValDataQueue{nullptr};
    void ensureMotorCurrentQueue() {
        if (motorCurrentAdcValDataQueue == nullptr)
            motorCurrentAdcValDataQueue = xQueueCreate(8, sizeof(int));
    }

private:
    float last_motor_current_amps_{0.0f};

    unsigned fsm_calls_{0};
    unsigned supply_acquires_{0};
    unsigned supply_releases_{0};
    bool     supply_ok_{true};
    bool  busy_{false};
    float restore_to_{0.0f};
    State state_;
    float target_position_{0.0f};
    bool  geometry_set_{false};
    float axle_{0.0f}, thick_{0.0f}, h_{0.0f};
    uint32_t open_s_{0}, close_s_{0};
    uint32_t open_slack_s_{0}, close_slack_s_{0};
};
