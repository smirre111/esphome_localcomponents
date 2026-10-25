#pragma once
// lora_client_internal.h -- what the listener's translation units share
// (lora_client.cpp and its split-out lora_client_*.cpp files). HUB-ONLY,
// never included by anything outside lora_client/.

#include <cinttypes>
#include <thread>
#include "esphome/components/lora_client/lora_client.h"
// Shared with the node firmware — the AEAD wire format now lives in exactly
// one place. See the banner in FrameCrypto.h.
#include "esphome/components/lora_client/FrameCrypto.h"
// MAC-2 (keys, KDF, Encrypt-then-CMAC, the session key store). See its banner.
#include "esphome/components/lora_client/HubLinkSecurity.h"
// hubframes::Hdr -> mac::MacHdr, shared with the tracker's copy sealer call (S4).
#include "esphome/components/lora_client/MacHdrCodec.h"
// S6: the one oneof -> mac::Port table.
#include "esphome/components/lora_client/PortTable.h"
// S8: MAC wire v1 -- the fixed header (shared) and the hub's bodies around it.
#include "esphome/components/lora_client/MacWire.h"
#include "esphome/components/lora_client/HubWire.h"

// The funnel rate arithmetic, so a ModeTest report is recomputed on the hub
// rather than trusted from the node (test-plan.md I1).
#include "esphome/components/lora_client/MacFunnel.h"
// Section 4.4: the pending-data bitmap the beacon carries.
#include "esphome/components/lora_client/PendingData.h"
// C2: the Class A window offsets the hub aims replies at.
#include "esphome/components/lora_client/ClassAWindows.h"
#include "esphome/components/lora_client/Scheduler.h"
#include "esphome/components/lora_tracker/lora_tracker.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esp_random.h"
#include <algorithm>
#include <map>
#include <memory>
#include <cstring>
#include <vector>
#include "esphome/components/lora_client/TimedGrid.h"

// ---------------------------------------------------------------------------
// MAC-2 lives in HubLinkSecurity.{h,cpp} since stage S2 (K_root/K_auth, the
// session KDF, Encrypt-then-CMAC, and the SessionKeyStore that is the single
// owner of every peer's session id, keys and generation). The wire is MAC
// wire v1 only (S8, docs/mac-wire-format.md): MacWire.h holds the header,
// HubWire the bodies; the v0 envelope path was deleted in W5.
// ---------------------------------------------------------------------------

static inline mac2::LinkSecurity &s_sec() { return mac2::LinkSecurity::hub(); }
static inline mac2::SessionKeyStore &s_keys() { return mac2::LinkSecurity::hub().store(); }

namespace esphome
{
  namespace lora_tracker
  {
    static const char *const TAG = "lora_client";

    namespace
    {
    // Marks an esp_timer callback as running for its whole body, so a stop on
    // the loop can wait for it (LORAListener::wait_callback_idle_, HB2).
    struct CallbackBusy
    {
      explicit CallbackBusy(std::atomic<bool> &b) : busy(b) { busy.store(true); }
      ~CallbackBusy() { busy.store(false); }
      std::atomic<bool> &busy;
    };
    }  // namespace
  }  // namespace lora_tracker
}  // namespace esphome
