// Externs that CmdDispatcher.cpp references but that live in other TUs of
// the production firmware (battery monitor task, motor current task,
// trigger_ota flag, task handles).
#include <freertos/FreeRTOS.h>
#include <cstdint>

void spawnTaskBatteryMonitor() {}
void spawnTaskMotorCurrentMonitor() {}

uint8_t trigger_ota = 0;
TaskHandle_t xHandleLoraPolling = nullptr;
TaskHandle_t xHandleMotor       = nullptr;

// --- globals the REAL LoraInterface.cpp expects the firmware to define ------
//
// isr_service_installed lives in main.cpp on the node: the GPIO ISR service is
// installed once for the whole firmware, and LoraInterface::setup() checks the
// flag rather than installing it a second time. Defined here so the file can be
// linked; false is the pre-install state, which is the one setup() is written
// for.
bool isr_service_installed = false;
