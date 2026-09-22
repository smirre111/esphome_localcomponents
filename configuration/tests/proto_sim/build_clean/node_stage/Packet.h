#pragma once
#include <freertos/FreeRTOS.h>
#include <string>

struct Packet
{
  uint8_t destAddress;   // destAddress address
  uint8_t destSubnet;    // destSubnet address
  uint8_t senderAddress; // senderAddress address
  uint8_t msgId;         // incoming msg ID
  uint8_t payloadLength; // incoming msg length
  uint8_t *payload;

  Packet()
      : destAddress(0),
        destSubnet(0),
        senderAddress(0),
        msgId(0),
        payloadLength(0)
  {
    payload = new uint8_t[128];
  }

  Packet(const Packet &other)
      : destAddress(other.destAddress),
        destSubnet(other.destSubnet),
        senderAddress(other.senderAddress),
        msgId(other.msgId),
        payloadLength(other.payloadLength)
  {
    payload = new uint8_t[128];
    for (int i = 0; i < 128; i++)
    {
      payload[i] = other.payload[i];
    }
  }

  Packet operator=(const Packet &other)
  {
    destAddress = other.destAddress;
    destSubnet = other.destSubnet;
    senderAddress = other.senderAddress;
    msgId = other.msgId;
    payloadLength = other.payloadLength;
    for (int i = 0; i < 128; i++)
    {
      payload[i] = other.payload[i];
    }
    return *this;
  }

  int length()
  {
    return 5 + payloadLength;
  }

  std::string toString()
  {
    return std::string((char *)payload);
  }
};
