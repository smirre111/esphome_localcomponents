// Host-side ESP-IDF ADC calibration shim.
//
// The battery and motor-current conversions go through the chip's eFuse
// calibration (`adc_cali_raw_to_voltage`), which is what fixed a ~11 % scale
// error: the old code used a full scale of 3.95/2 V for a channel configured at
// 6 dB, whose real full scale is ~2.2 V, so a 12.1 V pack read as ~10.7 V.
//
// IT DOES NOT MODEL THE SAR CURVE. A real calibration handle linearises the
// ADC's transfer function from per-chip eFuse data; there is no such data here
// and inventing a curve would make host numbers look meaningful when they are
// not. The shim reports NO calibration available by default, which is the
// branch production is written to survive (it falls back to a nominal full
// scale plus a per-board trim), and lets a test install a linear mV-per-count
// factor when it wants the conversion path exercised.
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct adc_cali_scheme_ctx *adc_cali_handle_t;

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *voltage_mv);

// ---- harness control (not part of the ESP-IDF API) ----
// Install a linear scheme: voltage_mv = raw * mv_per_count. Pass 0 to make
// every scheme creation fail, which is the default and the fallback path.
void proto_sim_adc_cali_set_linear(float mv_per_count);
void proto_sim_adc_cali_reset(void);

#ifdef __cplusplus
}
#endif
