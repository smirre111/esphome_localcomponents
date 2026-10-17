// Minimal LORAListener stubs for the real_lora_tracker target
// (tests/proto_sim/CMakeLists.txt), which compiles the REAL lora_tracker.cpp
// without the real lora_client.cpp.
//
// Since S4 the tracker no longer calls LORAListener::sealBurstCopyTag(): an
// encrypted frame carries its own MAC-2 copy sealer in TxPolicy, so the
// sealBurstCopyTag stub that used to live here is gone. What remains is the
// shadow pending-mask gather below.
#include "esphome/components/lora_client/lora_client.h"

namespace esphome
{
  namespace lora_tracker
  {
    // serviceBeacon gathers the SHADOW pending-mask inputs from every registered
    // listener. This target registers none, so the call is never reached at
    // runtime; the stub answers "listen" (the default-constructed inputs) for
    // the same reason sealBurstCopyTag answers false: the safe direction.
    pendingshadow::NodeInputs LORAListener::pending_shadow_inputs(int64_t)
    {
      return pendingshadow::NodeInputs{};
    }
  }
}
