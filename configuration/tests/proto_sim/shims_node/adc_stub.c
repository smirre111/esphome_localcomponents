// Implementation of the one-shot ADC shim. See esp_adc/adc_oneshot.h for why
// this models nothing analogue.
#include "esp_adc/adc_oneshot.h"

#include <stdlib.h>

struct adc_oneshot_unit_ctx {
    adc_unit_t unit;
    int        open;
    int        raw;
    esp_err_t  err;
};

// Two units, indexed by adc_unit_t, because the production code opens ADC_UNIT_1
// for the battery and ADC_UNIT_2 for the motor current and the two must not be
// confusable — that is exactly why the handles are named adc1_handle and
// adc2_handle there.
static struct adc_oneshot_unit_ctx g_units[2] = {
    {ADC_UNIT_1, 0, 2048, ESP_OK},
    {ADC_UNIT_2, 0, 2048, ESP_OK},
};

static int index_of(adc_unit_t u) { return (u == ADC_UNIT_2) ? 1 : 0; }

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,
                               adc_oneshot_unit_handle_t *out) {
    if (cfg == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    struct adc_oneshot_unit_ctx *u = &g_units[index_of(cfg->unit_id)];
    // The real driver refuses a second open of the same unit, and BOTH spawn
    // guards in frtosTasks.cpp exist because of it: without them the second
    // call returns here and the task then leaks. Reproduced rather than
    // smoothed over, so a test can hold those guards to their reason.
    if (u->open) return ESP_ERR_INVALID_STATE;
    u->open = 1;
    *out = u;
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h,
                                     adc_channel_t chan,
                                     const adc_oneshot_chan_cfg_t *cfg) {
    (void) chan;
    if (h == NULL || cfg == NULL) return ESP_ERR_INVALID_ARG;
    return h->open ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t chan,
                           int *out_raw) {
    (void) chan;
    if (h == NULL || out_raw == NULL) return ESP_ERR_INVALID_ARG;
    if (!h->open) return ESP_ERR_INVALID_STATE;
    if (h->err != ESP_OK) return h->err;
    *out_raw = h->raw;
    return ESP_OK;
}

esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    h->open = 0;
    return ESP_OK;
}

void proto_sim_adc_set_raw(adc_unit_t unit, int raw) {
    g_units[index_of(unit)].raw = raw;
}

void proto_sim_adc_set_error(adc_unit_t unit, esp_err_t err) {
    g_units[index_of(unit)].err = err;
}

int proto_sim_adc_open_units(void) {
    return (g_units[0].open ? 1 : 0) + (g_units[1].open ? 1 : 0);
}

void proto_sim_adc_reset(void) {
    for (int i = 0; i < 2; i++) {
        g_units[i].open = 0;
        g_units[i].raw  = 2048;
        g_units[i].err  = ESP_OK;
    }
}

// --- calibration -----------------------------------------------------------
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

struct adc_cali_scheme_ctx { float mv_per_count; };

static struct adc_cali_scheme_ctx g_cali = {0.0f};
static adc_cali_line_fitting_efuse_val_t g_efuse = ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF;

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *voltage_mv) {
    if (h == NULL || voltage_mv == NULL) return ESP_ERR_INVALID_ARG;
    if (h->mv_per_count <= 0.0f) return ESP_ERR_INVALID_STATE;
    *voltage_mv = (int) ((float) raw * h->mv_per_count);
    return ESP_OK;
}

esp_err_t adc_cali_scheme_line_fitting_check_efuse(adc_cali_line_fitting_efuse_val_t *out) {
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = g_efuse;
    return ESP_OK;
}

// Both creators fail unless a test installed a factor. Failing is the DEFAULT
// on purpose: production must survive a chip with no calibration data, and that
// fallback path is the one a host run should exercise unless a test says
// otherwise.
static esp_err_t make_scheme(adc_cali_handle_t *out) {
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (g_cali.mv_per_count <= 0.0f) return ESP_ERR_NOT_SUPPORTED;
    *out = &g_cali;
    return ESP_OK;
}

esp_err_t adc_cali_create_scheme_line_fitting(const adc_cali_line_fitting_config_t *cfg,
                                              adc_cali_handle_t *out) {
    (void) cfg; return make_scheme(out);
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                               adc_cali_handle_t *out) {
    (void) cfg; return make_scheme(out);
}

void proto_sim_adc_cali_set_linear(float mv_per_count) { g_cali.mv_per_count = mv_per_count; }
void proto_sim_adc_cali_set_efuse(adc_cali_line_fitting_efuse_val_t v) { g_efuse = v; }
void proto_sim_adc_cali_reset(void) {
    g_cali.mv_per_count = 0.0f;
    g_efuse = ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF;
}

// The handle is a file-static here, not an allocation, so deleting it is a
// no-op rather than a free. Production still calls these on its way out and a
// missing symbol is a link error.
esp_err_t adc_cali_delete_scheme_line_fitting(adc_cali_handle_t h) { (void) h; return ESP_OK; }
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h) { (void) h; return ESP_OK; }
