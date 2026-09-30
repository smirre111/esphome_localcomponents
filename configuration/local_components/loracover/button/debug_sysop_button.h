#pragma once

#include "esphome/components/button/button.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/loracover/loracover_base.h"

namespace esphome
{
  namespace loracov
  {

    // One button class for all three runtime debug toggles (DebugFlags.h on
    // the node, 2026-09-29) -- they differ only in which ClientOperation they
    // send, so a parameterised class avoids three near-identical copies of
    // TriggerOtaButton. See __init__.py for the YAML `op:` mapping.
    //
    // op_ is a raw int32_t, NOT ClientOperation -- this header is pulled in by
    // esphome.h (every platform gets aggregated there), and blinds.pb-c.h is
    // deliberately not visible at that scope (same reason lora_client.h's own
    // send_tracked_sysop_()/sendDebugSysop() take int32_t; see its top-of-file
    // note). __init__.py maps the YAML `op:` string straight to the
    // CLIENT_OPERATION__CMD_DEBUG_* integer value from blinds.proto.
    class DebugSysopButton : public esphome::lora_tracker::LORAClientNode, public button::Button, public Component
    {
    public:
      DebugSysopButton() = default;

      void set_op(int32_t op) { this->op_ = op; }

      void set_response(uint8_t *data, size_t len) override;

      void dump_config() override;

      void send_remote_config() override;

    protected:
      void press_action() override;

      int32_t op_{0};  // overwritten by set_op() before use
    };

  } // namespace loracov
} // namespace esphome
