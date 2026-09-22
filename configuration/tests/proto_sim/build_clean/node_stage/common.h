#pragma once
#include "esp_log.h"

#include "driver/gpio.h"


#define TICKS_TO_WAIT 1



#define POLL_CONT 1
#define CONFIG_EXAMPLE_EXT1_WAKEUP 1
#define CONFIG_EXAMPLE_EXT1_WAKEUP_PIN_1 34
#define CONFIG_EXAMPLE_EXT1_WAKEUP_PIN_1_SEL_34 1
#define CONFIG_EXAMPLE_EXT1_WAKEUP_PIN_2 35
#define CONFIG_EXAMPLE_EXT1_WAKEUP_PIN_2_SEL_35 1
#define CONFIG_EXAMPLE_EXT1_WAKEUP_PIN_3 4
#define CONFIG_EXAMPLE_EXT1_WAKEUP_PIN_3_SEL_4 1
#define CONFIG_ESP_EXT1_WAKEUP_ANY_HIGH 1
#define CONFIG_EXAMPLE_EXT1_WAKEUP_MODE 1
#define CONFIG_EXAMPLE_GPIO_WAKEUP_PIN 0
#define CONFIG_EXAMPLE_GPIO_WAKEUP_HIGH_LEVEL 1


#define LORA_POLLING 0
#define RX_CONT 0


typedef uint8_t blinds_syscmd_base_t;



enum MotorCmd_t
{
  MOTCMD_IDLE,
  MOTCMD_FULL_UP,
  MOTCMD_FULL_DOWN,
  MOTCMD_STEP_UP,
  MOTCMD_STEP_DOWN,
  MOTCMD_STOP,
  MOTCMD_TIMER
};

enum BlindsState_t
{
  BLINDS_IDLE,
  BLINDS_STEP_UP,
  BLINDS_STEP_DOWN,
  BLINDS_FULLY_OPENING,
  BLINDS_FULLY_CLOSING
};

enum ha_blinds_state_t
{
  BLINDS_MQTT_OPEN,
  BLINDS_MQTT_OPENING,
  BLINDS_MQTT_CLOSING,
  BLINDS_MQTT_CLOSED,
  BLINDS_MQTT_UNAVAILABLE,
  BLINDS_MQTT_UNKNOWN

};




struct BlindsOpCmd
{
  static const blinds_syscmd_base_t SYSCMD_OPEN = '1';
  static const blinds_syscmd_base_t SYSCMD_CLOSE = '2';
  static const blinds_syscmd_base_t SYSCMD_STOP = '3';
  static const blinds_syscmd_base_t SYSCMD_IDLE = '4';
};

struct BlindsSysCmd
{
  static const blinds_syscmd_base_t SYSCMD_ENABLE_WIFI = '1';
  static const blinds_syscmd_base_t SYSCMD_DISABLE_WIFI = '2';
  static const blinds_syscmd_base_t SYSCMD_OTA = '3';
  static const blinds_syscmd_base_t SYSCMD_STATUS = '4';
  static const blinds_syscmd_base_t SYSCMD_SLEEP = '5';
};

struct BlindsStatusCmd
{
  static const blinds_syscmd_base_t SYSCMD_BATTERY = '1';
  static const blinds_syscmd_base_t SYSCMD_POSITION = '2';
  static const blinds_syscmd_base_t SYSCMD_AVAILABLE = '3';
  static const blinds_syscmd_base_t SYSCMD_REGISTER = '4';
  static const blinds_syscmd_base_t SYSCMD_ACK = '5';
  // P2: wake beacon — sent on every boot/wake so the hub learns why we woke,
  // what schedule version we hold, and what our clock reads.
  static const blinds_syscmd_base_t SYSCMD_BEACON = '6';
};

static const gpio_num_t motSupplyEn = gpio_num_t(4); //Is an RTC GPIO in ESP32 WROOM 32UE

static const gpio_num_t ctrlButtonUpPin = gpio_num_t(36); //Old Is an RTC GPIO in ESP32 WROOM 32UE
static const gpio_num_t ctrlButtonDownPin = gpio_num_t(35); //Is an RTC GPIO in ESP32 WROOM 32UE
static const gpio_num_t ctrlButtonStopPin = gpio_num_t(34); //Is an RTC GPIO in ESP32 WROOM 32UE

static const gpio_num_t motorIn1Pin = gpio_num_t(21); // Is not an RTC GPIO in ESP32 WROOM 32UE
static const gpio_num_t motorIn2Pin = gpio_num_t(22); // Is not an RTC GPIO in ESP32 WROOM 32UE
static const gpio_num_t motorPwmPin = gpio_num_t(23); // Is not an RTC GPIO in ESP32 WROOM 32UE

// ---------------------------------------------------------------------------
// Shared LoRa RX buffer geometry.
//
// LoraInterface allocates a buffer from its pool and hands the SAME POINTER to
// CmdDispatcher — there is no copy between the layers (the logs show
// "queued buffer 0x3ffc0af4" followed by "Processing buffer 0x3ffc0af4"). Both
// therefore have to agree on the size.
//
// These were previously two independent private constants, one per class, with
// equal values and nothing tying them together. Raising one for a larger frame
// while missing the other would overrun the layer that was not updated, and
// nothing in the build would say so.
//
// 256 covers the SX1278's 255-byte maximum payload.
// ---------------------------------------------------------------------------
static constexpr int kLoraBufferSize = 256;
static constexpr int kLoraPoolSize   = 5;
