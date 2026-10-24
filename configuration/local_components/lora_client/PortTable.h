#pragma once
// PortTable.h -- the hub's ONE table from an uplink's case to its mac::Port
// (stage S6, docs/mac-separation-layering-plan.md; review 5.1). With MAC wire v1
// the port is on the wire (macwire PORT); this classifies the internal case the
// body decoded to, and must agree with it:
//   Session  -- REGISTER: MAC-2 session lifecycle
//   Mac0Mgmt -- MAC ACK (ARQ + phase report), MAC control (ping echo),
//               ModeTest report, GridSync request: consumed by the MAC
//               handlers, never handed to an application child
//   App      -- everything else (AVAIL, STATE, POSITION, BEACON): the
//               listener's application handlers, then the loracover children
//
// Hub-only (the node classifies its downlinks with its own table).

#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/MacSap.h"

namespace porttable
{

inline mac::Port uplinkPort(hubframes::UpCase c)
{
    switch (c)
    {
    case hubframes::UL_REGISTER:
        return mac::Port::Session;
    case hubframes::UL_ACK:
    case hubframes::UL_MACCONTROL:
    case hubframes::UL_MODETESTREPORT:
    case hubframes::UL_GRIDSYNCREQUEST:
        return mac::Port::Mac0Mgmt;
    default:
        return mac::Port::App;
    }
}

}  // namespace porttable
