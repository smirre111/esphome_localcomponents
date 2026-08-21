// Stub SystemCtrl — captures CLIENTCONFIG / COVERCONFIG application and
// stands in for LittleFS persistence + WiFi/OTA control.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

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

    // WiFi / OTA / deep-sleep — no-ops.
    void setupWiFi()    {}
    void shutdownWiFi() {}
    void setupOTA()     {}
    void enterDeepsleep() {}

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
};
