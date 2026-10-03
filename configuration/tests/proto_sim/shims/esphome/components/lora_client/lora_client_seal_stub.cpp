// Minimal stub for LORAListener::sealBurstCopyTag(), used ONLY by the
// real_lora_tracker target (tests/proto_sim/CMakeLists.txt).
//
// That target compiles the REAL lora_tracker.cpp against a STUBBED hub
// client side (shims/esphome/components/lora_tracker/lora_tracker.cpp
// provides the fake LORATracker; there is no fake LORAListener), so it
// never links the real lora_client.cpp and has no other source of this
// symbol. sendPacketBurst() now calls it unconditionally when sealing an
// encrypted burst copy (Tier 3, mac-separation-implementation-plan.md
// section 2(b)) — the call is a direct, non-virtual function call, so the
// linker needs a definition to exist even on a path these tracker-only
// tests never actually execute at runtime (clients_ is empty here; this
// is reachable only through a real LORAClient this target never
// constructs).
//
// Always failing is the correct behaviour for a stub: sendPacketBurst()
// already treats a failed seal as "drop this copy, do not send it
// half-sealed", which is exactly right when there is no real session to
// seal under.
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace lora_tracker
  {
    bool LORAListener::sealBurstCopyTag(::EncryptedPayload *, const ::LoraHeader *)
    {
      return false;
    }
  }
}
