#pragma once
// PortTable.h -- the hub's ONE table from an uplink's protobuf oneof case to its
// mac::Port (stage S6, docs/mac-separation-layering-plan.md; review 5.1).
//
// Until the wire stage (S8) the port is not carried; it is COMPUTED from the
// oneof case here, and nowhere else:
//   Session  -- REGISTER, LOGIN: MAC-2 session lifecycle
//   Mac0Mgmt -- CommandAck (ARQ + phase report), MAC control (ping echo),
//               ModeTest report, GridSync request: consumed by the MAC
//               handlers, never handed to an application child
//   App      -- everything else (AVAIL, STATE, POSITION, BEACON): the
//               listener's application handlers, then the loracover children
//
// Hub-only (the node classifies its downlinks with its own table).

#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/lora_client/MacSap.h"

namespace porttable
{

inline mac::Port uplinkPort(LoraClientResponseMessage__ProtoCase c)
{
    switch (c)
    {
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_LOGIN:
        return mac::Port::Session;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MODETESTREPORT:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_GRIDSYNCREQUEST:
        return mac::Port::Mac0Mgmt;
    default:
        return mac::Port::App;
    }
}

}  // namespace porttable
