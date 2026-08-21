#pragma once
#include <freertos/FreeRTOS.h>
#include <cstdint>

// CmdDispatcher.h holds two Packet members (rxPacket, txPacket) but
// nothing in the code paths we exercise actually touches them — only the
// type needs to exist.
struct Packet {
    uint8_t  destAddress{0};
    uint8_t  destSubnet{0};
    uint8_t  senderAddress{0};
    uint8_t  msgId{0};
    uint8_t  payloadLength{0};
    uint8_t* payload{nullptr};
};
