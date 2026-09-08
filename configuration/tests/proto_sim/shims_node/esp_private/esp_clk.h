// esp_private/esp_clk.h shim: the CPU frequency, which the ModeTest report
// carries so a timing number can never be read without knowing what clock it
// was taken on.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
// Production returns the live frequency, which scales 40-240 MHz. The host has
// no such thing, so it reports the pinned maximum — a test asserting on this
// value would be asserting on the harness, not on the node.
static inline int esp_clk_cpu_freq(void) { return 240 * 1000 * 1000; }
#ifdef __cplusplus
}
#endif
