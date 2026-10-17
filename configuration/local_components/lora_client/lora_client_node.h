#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/blindsproto/blinds.pb-c.h"   // AppIndication carries a decoded uplink
#include "esphome/components/lora_client/MacSap.h"

#include <array>
#include <string>
#include <vector>

namespace esphome
{
  namespace lora_tracker
  {

    class LORAListener;
    class LORAClient;

    // S6 (docs/mac-separation-layering-plan.md): an uplink for the
    // application, already demultiplexed (mac::Port::App, PortTable.h), from
    // THIS child's node (the listener's address filter and replay window have
    // passed) and authenticated whenever a session exists. Children no longer
    // see wire bytes, run their own address filter or re-parse the envelope.
    struct AppIndication
    {
      mac::MacHdr                        hdr{};            // the outer header, decoded
      const ::LoraClientResponseMessage *msg{nullptr};     // the decoded payload (msg->header may be null)
      bool                               authentic{false}; // false only on the pre-session plaintext bootstrap
    };

    class LORAClientNode
    {
    public:
      // An application uplink for this node (see AppIndication). Default: ignore.
      virtual void on_app_indication(const AppIndication &ind) { (void) ind; }
      // The node REGISTERed (mac::Port::Session): push this child's own
      // configuration if it has one. Default: nothing to push.
      virtual void on_register() {}
      virtual void send_remote_config() = 0;

      // P2: the node reported its own clock in a wake beacon; offset_s is
      // node_epoch - hub_epoch (positive = node runs ahead).  Default no-op so
      // only the platforms that surface it need to implement it.
      virtual void on_clock_offset(float offset_s) { (void) offset_s; }

      virtual void loop() {}
      void set_lora_client_parent(LORAListener *parent) { this->parent_ = parent; }

    protected:
      LORAListener *parent_{nullptr};
    };

  }
}