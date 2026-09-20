// Host-side ESP-IDF one-shot ADC shim.
//
// frtosTasks.cpp needs this to compile at all, and until it did, the whole
// interrupt path — the DIO0/DIO1 handlers, the TX_DONE branch that pairs with
// LoraInterface::last_tx_len_, and the ISR timestamps every phase measurement
// rests on — was unreachable by any test in either repository. That is the last
// of T-1.
//
// IT DOES NOT MODEL AN ADC. The two ADC users in that file are the motor
// current sensor and the battery monitor, and neither is protocol: what the
// shim owes them is a surface that compiles and a reading a test can choose.
// The real analogue behaviour belongs on a bench, next to HW-1's numbers.
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADC_UNIT_1 = 0,
    ADC_UNIT_2 = 1,
} adc_unit_t;

typedef enum {
    ADC_ULP_MODE_DISABLE = 0,
    ADC_ULP_MODE_FSM     = 1,
    ADC_ULP_MODE_RISCV   = 2,
} adc_ulp_mode_t;

typedef enum {
    ADC_ATTEN_DB_0   = 0,
    ADC_ATTEN_DB_2_5 = 1,
    ADC_ATTEN_DB_6   = 2,
    ADC_ATTEN_DB_12  = 3,
} adc_atten_t;

typedef enum {
    ADC_BITWIDTH_DEFAULT = 0,
    ADC_BITWIDTH_9       = 9,
    ADC_BITWIDTH_10      = 10,
    ADC_BITWIDTH_11      = 11,
    ADC_BITWIDTH_12      = 12,
} adc_bitwidth_t;

typedef enum {
    ADC_CHANNEL_0 = 0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4,     ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7,
    ADC_CHANNEL_8,     ADC_CHANNEL_9,
} adc_channel_t;

// The clock source is an enum in the real header and is cast from
// ADC_DIGI_CLK_SRC_DEFAULT at both call sites, so both names have to exist.
typedef int adc_oneshot_clk_src_t;
#define ADC_DIGI_CLK_SRC_DEFAULT 0

typedef struct adc_oneshot_unit_ctx *adc_oneshot_unit_handle_t;

typedef struct {
    adc_unit_t            unit_id;
    adc_oneshot_clk_src_t clk_src;
    adc_ulp_mode_t        ulp_mode;
} adc_oneshot_unit_init_cfg_t;

typedef struct {
    adc_atten_t    atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,
                               adc_oneshot_unit_handle_t *out);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h,
                                     adc_channel_t chan,
                                     const adc_oneshot_chan_cfg_t *cfg);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t chan,
                           int *out_raw);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h);

// ---- harness control (not part of the ESP-IDF API) ----
// What the next adc_oneshot_read on this unit returns, and whether it fails.
// The battery monitor's error branch skips a cycle rather than publishing a
// garbage voltage, so a test has to be able to produce the failure.
void      proto_sim_adc_set_raw(adc_unit_t unit, int raw);
void      proto_sim_adc_set_error(adc_unit_t unit, esp_err_t err);
// How many units are currently open. Both spawn guards exist because opening a
// unit twice returns ESP_ERR_INVALID_STATE and then leaks the task.
int       proto_sim_adc_open_units(void);
void      proto_sim_adc_reset(void);

#ifdef __cplusplus
}
#endif
