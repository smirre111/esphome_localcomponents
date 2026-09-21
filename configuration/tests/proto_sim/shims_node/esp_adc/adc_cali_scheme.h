// Host-side shim for the calibration SCHEMES. See adc_cali.h for why neither
// models a real transfer function.
//
// Both schemes are declared because production tries them in order and the
// ESP32 supports only line fitting — the #if on these macros is itself part of
// what the code does, so the macros have to exist with the ESP32's values
// rather than being assumed away.
#pragma once

#include "esp_adc/adc_cali.h"

#ifdef __cplusplus
extern "C" {
#endif

// The ESP32 has line fitting and not curve fitting. These are the values the
// real headers carry for this target; production branches on them.
#define ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED  1
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 0

typedef enum {
    ADC_CALI_LINE_FITTING_EFUSE_VAL_EFUSE_VREF = 0,
    ADC_CALI_LINE_FITTING_EFUSE_VAL_EFUSE_TP   = 1,
    ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF = 2,
} adc_cali_line_fitting_efuse_val_t;

typedef struct {
    adc_unit_t     unit_id;
    adc_atten_t    atten;
    adc_bitwidth_t bitwidth;
    uint32_t       default_vref;
} adc_cali_line_fitting_config_t;

typedef struct {
    adc_unit_t     unit_id;
    adc_channel_t  chan;
    adc_atten_t    atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;

esp_err_t adc_cali_scheme_line_fitting_check_efuse(adc_cali_line_fitting_efuse_val_t *out);
esp_err_t adc_cali_create_scheme_line_fitting(const adc_cali_line_fitting_config_t *cfg,
                                              adc_cali_handle_t *out);
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                               adc_cali_handle_t *out);

esp_err_t adc_cali_delete_scheme_line_fitting(adc_cali_handle_t handle);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle);

// ---- harness control ----
// What check_efuse reports. Default is DEFAULT_VREF: no per-chip data, which is
// the case production's fallback exists for.
void proto_sim_adc_cali_set_efuse(adc_cali_line_fitting_efuse_val_t v);

#ifdef __cplusplus
}
#endif
