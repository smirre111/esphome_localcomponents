// Stub SystemCtrl — captures CLIENTCONFIG / COVERCONFIG application and
// stands in for LittleFS persistence + WiFi/OTA control.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

// P3: the real Scheduler.h is dependency-free, so the shim uses the production
// header directly rather than mirroring the Entry type.
#include "Scheduler.h"

class SystemCtrl {
public:
    void setAddress(uint8_t addr, uint8_t subnt) { addr_ = addr; subnt_ = subnt; }
    uint8_t getConfigAddress() const             { return addr_; }
    uint8_t getConfigSubnet()  const             { return subnt_; }

    void setHostname(const char* name, size_t len) { hostname_.assign(name, len); }
    void setSleepDuration(uint64_t s)              { sleep_s_ = s; }
    void setTimes(uint32_t open_s, uint32_t close_s) { open_s_ = open_s; close_s_ = close_s; }
    void setGeometry(float h, float a, float t)    { h_ = h; a_ = a; t_ = t; geom_ = true; }
    // Slat-slack head/tail times (v1.0.10) and the configurable battery
    // force-send interval (v1.0.12).  interval_s == 0 means "unset" and is
    // ignored by production, which keeps its current value.
    void setSlack(uint32_t open_slack_s, uint32_t close_slack_s) {
        open_slack_s_ = open_slack_s; close_slack_s_ = close_slack_s;
    }
    void setBatteryInterval(uint32_t interval_s) {
        if (interval_s != 0) battery_interval_s_ = interval_s;
    }
    uint32_t getConfigBatteryInterval() const      { return battery_interval_s_; }
    void setRegistered()                           { registered_ = true; }
    bool getRegistered() const                     { return registered_; }

    // LittleFS persistence — no-ops on host (test inspects the in-RAM
    // setters directly).
    void mountLittleFS()      {}
    void saveConfiguration()  {}
    void unmountLittleFS()    {}

    // WiFi / OTA — no-ops.
    void setupWiFi()    {}
    void shutdownWiFi() {}
    void setupOTA()     {}

    // Deep sleep: RECORDED, not ignored.  This used to be a bare no-op, which
    // meant the entire sleep path — the thing automatic mode depends on most —
    // had zero host coverage.  A change that called enterDeepsleep() from the
    // wrong place therefore passed the suite and only failed on hardware.
    // Counting the calls lets tests assert WHEN sleep is requested, and just as
    // importantly when it must not be.
    void enterDeepsleep() { deepsleep_calls_++; }
    int  deepsleep_calls() const { return deepsleep_calls_; }
    void reset_deepsleep_calls()  { deepsleep_calls_ = 0; }

    // Test inspection.
    const std::string& hostname() const { return hostname_; }
    uint64_t sleep_duration_s()  const { return sleep_s_; }
    uint32_t open_time_s()       const { return open_s_; }
    uint32_t close_time_s()      const { return close_s_; }
    bool     geometry_set()      const { return geom_; }
    float    height_mm()         const { return h_; }
    float    axle_mm()           const { return a_; }
    float    thickness_mm()      const { return t_; }
    uint32_t open_slack_s()      const { return open_slack_s_; }
    uint32_t close_slack_s()     const { return close_slack_s_; }

    // ---- P3: schedule / mode ----
    void setSchedule(uint32_t version, uint8_t mode,
                     uint32_t interactiveTimeout_s, uint32_t checkinInterval_s,
                     uint32_t beaconLead_s, uint32_t postEventWindow_s,
                     uint32_t catchupWindow_s,
                     const sched::Entry *entries, uint8_t count) {
        sched_version_ = version;
        auto_mode_     = mode != 0;
        // Mirrors production: zero handling follows what the proto DOCUMENTS
        // per field. interactiveTimeout / checkinInterval / catchupWindow all
        // have meaningful zeros ("stay interactive", "no check-in", "never
        // replay"); beaconLead and postEventWindow do not, and a 0 there would
        // silently defeat the wake-early-then-act behaviour.
        interactive_timeout_s_ = interactiveTimeout_s;
        checkin_interval_s_    = checkinInterval_s;
        catchup_window_s_      = catchupWindow_s;
        if (beaconLead_s)      beacon_lead_s_       = beaconLead_s;
        if (postEventWindow_s) post_event_window_s_ = postEventWindow_s;

        entry_count_ = 0;
        if (entries) {
            for (uint8_t i = 0; i < count && i < sched::kMaxEntries; i++) {
                if (entries[i].minuteOfDay >= sched::kMinutesPerDay) continue;
                entries_[entry_count_++] = entries[i];
            }
        }
    }
    void setAutoMode(bool on)          { auto_mode_ = on; }
    bool getAutoMode()                 { return auto_mode_; }
    uint32_t getSchedVersion()         { return sched_version_; }
    uint32_t getInteractiveTimeout()   { return interactive_timeout_s_; }
    uint32_t getCheckinInterval()      { return checkin_interval_s_; }
    uint32_t getBeaconLead()           { return beacon_lead_s_; }
    uint32_t getPostEventWindow()      { return post_event_window_s_; }
    uint32_t getCatchupWindow()        { return catchup_window_s_; }
    const sched::Entry *getEntries()   { return entries_; }
    uint8_t getEntryCount()            { return entry_count_; }
    bool hasUsableSchedule() {
        for (uint8_t i = 0; i < entry_count_ && i < sched::kMaxEntries; i++)
            if (entries_[i].valid()) return true;
        return false;
    }

private:
    uint8_t   addr_{0}, subnt_{0};
    bool      registered_{false};
    std::string hostname_;
    uint64_t  sleep_s_{0};
    uint32_t  open_s_{0}, close_s_{0};
    bool      geom_{false};
    float     h_{0.0f}, a_{0.0f}, t_{0.0f};
    uint32_t  open_slack_s_{0}, close_slack_s_{0};
    uint32_t  battery_interval_s_{900};  // production default: 15 min
    int       deepsleep_calls_{0};

    // P3 — defaults mirror the production struct Config.
    bool      auto_mode_{false};
    uint32_t  sched_version_{0};
    uint32_t  interactive_timeout_s_{1800};
    uint32_t  checkin_interval_s_{21600};
    uint32_t  beacon_lead_s_{30};
    uint32_t  post_event_window_s_{20};
    uint32_t  catchup_window_s_{1800};
    uint8_t   entry_count_{0};
    sched::Entry entries_[sched::kMaxEntries]{};
};
