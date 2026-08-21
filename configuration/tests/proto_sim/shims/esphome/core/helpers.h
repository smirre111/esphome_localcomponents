// Host-side shim for ESPHome's helpers.h. Only the symbols used by
// lora_client.{h,cpp} are provided.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esphome {

// ESPHome uses its own optional<T>; std::optional is API-compatible enough
// for our purposes (has_value, *, operator bool, value()).
template <typename T> using optional = std::optional<T>;

// Production helpers.h provides make_unique<T>. We forward to std.
using std::make_unique;

// Production HAL exposes millis() (ms since boot). The cover .cpp uses
// it for the busy-state timeout. Host-side shim returns 0 — tests that
// drive timing inject via SimClock directly.
inline uint32_t millis() { return 0; }

// Format a 6-byte MAC as "AA:BB:CC:DD:EE:FF" into the provided buffer
// (production helper).
inline void format_mac_addr_upper(const uint8_t mac[6], char* out_18) {
    std::snprintf(out_18, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Minimal ESPPreferenceObject — a per-key in-RAM blob with load/save that
// returns bool to mirror the production API. Test fixtures can inject and
// inspect the blob via the global PreferenceStore (declared below).
class ESPPreferenceObject {
public:
    ESPPreferenceObject() = default;
    explicit ESPPreferenceObject(std::vector<uint8_t>* slot) : slot_(slot) {}

    template <typename T> bool save(const T* value) {
        if (!slot_) return false;
        slot_->assign(reinterpret_cast<const uint8_t*>(value),
                      reinterpret_cast<const uint8_t*>(value) + sizeof(T));
        return true;
    }

    template <typename T> bool load(T* out) const {
        if (!slot_ || slot_->size() != sizeof(T)) return false;
        std::memcpy(out, slot_->data(), sizeof(T));
        return true;
    }

private:
    std::vector<uint8_t>* slot_{nullptr};
};

} // namespace esphome
