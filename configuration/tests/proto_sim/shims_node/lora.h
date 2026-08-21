// LoRa SX1278 driver stub — CmdDispatcher.cpp only references
// lora_packetRssi() during received-packet logging.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
inline int lora_packetRssi(void)  { return -42; }
inline float lora_packetSnr(void) { return 10.0f; }
#ifdef __cplusplus
}
#endif
