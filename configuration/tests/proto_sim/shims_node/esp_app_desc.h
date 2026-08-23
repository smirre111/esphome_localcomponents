#pragma once
// Minimal esp_app_desc shim.
//
// The node derives the firmware version it announces in its wake beacon from
// the running image rather than from a hand-maintained constant (the constant
// drifted: a node running 1.0.17 announced 10014 for three releases). The
// harness has no image, so this returns a fixed, deliberately distinctive
// version that a test can assert on end to end.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char version[32];
    char project_name[32];
} esp_app_desc_t;

// Fixed at "9.8.7" -> 90807. Chosen so a test failure cannot be confused with a
// real version number that happens to be current.
const esp_app_desc_t *esp_app_get_description(void);

#ifdef __cplusplus
}
#endif
