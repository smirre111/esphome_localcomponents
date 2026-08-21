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
