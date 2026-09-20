// Externs that CmdDispatcher.cpp references but that live in other TUs of
// the production firmware (battery monitor task, motor current task,
// trigger_ota flag, task handles).
#include <freertos/FreeRTOS.h>
#include <cstdint>

// The REAL definitions live in frtosTasks.cpp. A target that compiles that
// file must not also get these, or the archive carries two definitions of each
// and which one the link picks is decided by member-extraction order — so
// CmdDispatcher's call could reach either, unknowably. Observed: it linked
// silently with both present.
#ifndef PROTO_SIM_HAVE_REAL_FRTOSTASKS
void spawnTaskBatteryMonitor() {}
void spawnTaskMotorCurrentMonitor() {}
#else
// --- main.cpp's globals, for the target that compiles frtosTasks.cpp --------
//
// The task bodies reach the rest of the firmware through these, and on the
// node they are defined in main.cpp. Nothing defined them here because nothing
// compiled the file that uses them; a test in this target IS main.cpp, so it
// points them at its own objects in SetUp.
//
// Null by default, which is what the handlers' own guards are written for —
// every use of cmdDispatcher and loraIf in the interrupt path is
// null-checked, and a test that wires up only what it needs is exercising
// those guards rather than working around them.
class MotorCtrl;
class SystemCtrl;
class LoraInterface;
class CmdDispatcher;
MotorCtrl     *motCtrl       = nullptr;
SystemCtrl    *sysCtrl       = nullptr;
LoraInterface *loraIf        = nullptr;
CmdDispatcher *cmdDispatcher = nullptr;

volatile SemaphoreHandle_t periodic_task_semaphore = nullptr;
// intQueueDio0 / intQueueDio1 are NOT here: LoraInterface.cpp defines them,
// and it is compiled into this same target. Defining them again is a multiple
// definition, which is how I found out where they live.
TaskHandle_t xHandleCurrentMeasurement = nullptr;
TaskHandle_t xHandleBatteryMeasurement = nullptr;
#endif

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
