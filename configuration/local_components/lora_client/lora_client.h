#pragma once

#include <esp_timer.h>
#include "esphome/components/lora_client/TimedModePolicy.h"
// The grid geometry the policy is judged against — kGuardUs, in particular.
#include "esphome/components/lora_client/TimedGrid.h"
// The placement contract: kPrepareLeadUs bounds how close to its instant a
// placed frame may be handed over, which is what makes a Class A window
// reachable or not.
#include "esphome/components/lora_client/TxQueue.h"
// Mode C wake-clock fit.
#include "esphome/components/lora_client/WakeClockFit.h"
// One node's login-to-settled window at a time, over the hub's single downlink.
#include "esphome/components/lora_client/OnboardingGate.h"
#include "esphome/components/lora_client/RegisterFreshness.h"
// How often an automatic-mode node needs its wall clock resynced.
#include "esphome/components/lora_client/TimeSyncPolicy.h"
// Hub-only: the first ack wait of an optimistic single shot, and the shadow
// pending-data mask (computed and logged, never transmitted).
#include "esphome/components/lora_client/SingleShotAckWait.h"
#include "esphome/components/lora_client/DeliveryPolicy.h"
#include "esphome/components/lora_client/DeliveryService.h"
#include "esphome/components/lora_client/HubSession.h"
#include "esphome/components/lora_client/NodeLinkTiming.h"
#include "esphome/components/lora_client/ModeBSupervisor.h"
#include "esphome/components/lora_client/SleepModel.h"
// MAC-2: keys, KDF, Encrypt-then-CMAC and the session key store (stage S2).
#include "esphome/components/lora_client/HubLinkSecurity.h"
// MAC-1: the shared frame counter (node-owned, vendored, drift-gated; stage S3).
#include "esphome/components/lora_client/Sequencer.h"
#include "esphome/components/lora_client/FcntReservation.h"
#include "esphome/components/lora_client/BenchStreams.h"
// S8: the MAC wire v1 fixed header (vendored from the node).
#include "esphome/components/lora_client/MacWire.h"
// S8 / W5c: the internal frame types and the v1 body codec.
#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/HubWire.h"
#include <memory>
#include "esphome/components/lora_client/PendingShadow.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/lora_client/lora_client_node.h"

#include "esphome/components/homeassistant/time/homeassistant_time.h"

#include <esp_bt_defs.h> //For esp_bd_addr_t

#include <psa/crypto.h>

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Generated protobuf-c type, declared here rather than including
// blinds.pb-c.h: this header is pulled in by every loracover platform, and the
// stub header is large and C-linkage. Only a pointer to it is needed.
#include "esphome/components/lora_client/ScheduleText.h"

struct NodeWakeBeacon;
struct MacControl;                  // MAC-0 ping / echo, taken by pointer
struct ModeTestReport;              // ModeTest results, taken by pointer
struct GridSyncRequest;             // a node asking for its grid again, by pointer
struct GridSync;                    // the grid being published, filled by fill_grid_sync_
struct PhaseReport;                 // §4.6's promotion evidence, by pointer
struct TimeSync;                    // filled by fill_timesync_
struct ClientConfig;                // filled by fill_client_config_
struct CoverConfig;                 // send_cover_config takes it by reference

namespace esphome
{
  // Forward declaration so LORAListener can hold an optional "command failed"
  // binary sensor without forcing the binary_sensor component on every user of
  // this header (the full include lives in lora_client.cpp).
  namespace binary_sensor { class BinarySensor; }
  // Same reason: the optional "Sleep Status" text sensor.
  namespace text_sensor { class TextSensor; }
  // P4b: forward-declared for the same reason — this header is included by
  // every loracover platform, so pulling in switch.h would be gratuitous.
  namespace switch_ { class Switch; }

  namespace lora_tracker
  {
    // The hub/controller's own LoRa address, used as senderAddress on every
    // downlink frame.  A small value keeps a 1-byte protobuf varint; nodes use
    // 17/18, broadcast is 0xFF and subnet 0xFE, so 0x01 is free.  (Nodes learn
    // the hub address dynamically from the received senderAddress, so this can
    // change hub-side without a node change.)
    //
    // Six `TODO: Use unique address` comments used to sit at the call sites —
    // four inside commented-out code, two on live lines, and not on all four
    // live uses — so they read as unfinished work rather than a decision.
    // There is exactly one hub; if a second ever shares the air this becomes
    // per-hub state. Until then it is deliberate.
    static constexpr uint8_t kHubAddress = 0x01;

    class LORATracker;
    class LORAClientNode;
    class LORAListener;
    struct TxPolicy;


    struct LORAClientRestoreState
    {
      uint8_t  version;           // must equal LORAListener::kRestoreStateVersion; otherwise discarded
      uint32_t rx_message_id;
      uint32_t tx_message_id;
      bool     logged_in;
      uint32_t last_sleep_epoch;  // Unix timestamp when enterSleep() was last called; 0 = unknown
      // What ClientConfig's fields hashed to when this was saved. A hub reboot
      // used to reset config_synced_ to false in RAM regardless, so EVERY node
      // was asked to REGISTER again on the hub's next login — a full round trip
      // for a node whose config had not changed at all (power-rf-review-
      // 2026-09-27.md finding 2). Comparing this against the CURRENT hash at
      // setup() lets an unchanged node skip straight to "already in sync"; a
      // real YAML edit changes the hash and still forces the push.
      uint32_t config_hash;

      void apply(LORAListener *listener);

    } __attribute__((packed));

 

    // THE CONTROL CHANNEL'S COMMAND: what the cover op / sysop in flight IS. The
    // application's data; how it is delivered is the delivery service's
    // (DeliveryService's private slot).
    struct ControlCommand
    {
      // What encode_ctl_ builds the command from.
      enum class Kind : uint8_t { COVER, SYSOP };
      Kind     kind{Kind::COVER};
      // Which LOGICAL command this is, for txqueue::SupersedeTable. Bumped once
      // per command from Home Assistant — never per retransmission, which must
      // stay under the generation it belongs to.
      //
      // Deliberately NOT msgid, even though msgid is also monotonic: a LoginMsg
      // ZEROES the message counters, so a generation taken from msgid would go
      // backwards after a re-login and the first command afterwards would be
      // dropped as stale by frames queued before it. Starts at 1 because 0 is
      // the table's "takes no part in this" value.
      uint32_t generation{1};
      // Stored operation, replayed on each retransmission with a fresh msgid.
      uint32_t covop_case{0};
      int32_t  operation{0};
      float    position{0.0f};
      int32_t  sysop{0};         // ClientOperation value when kind == SYSOP
      // Security review finding 1: a cover op / sysop issued while
      // relogin_pending is true would otherwise pack plaintext (session not
      // confirmed yet) and get stored — then every retry is rejected by the
      // node's own plaintext-gate (it already holds a session), and after
      // kOpMaxRetries the hub tears down the session it just rebuilt. Set by
      // send_cover_operation()/send_tracked_sysop_() when deferring instead
      // of sending; consumed by confirm_session_(), which actually sends it
      // once encryption is possible.
      bool     deferred_until_login{false};
      // True from "retries exhausted, no ack" until the node is next heard
      // (CMD_SLEEP only; see on_sleep_unconfirmed_).
      bool     sleep_unconfirmed{false};
    };

    class LORAListener : public EntityBase, public Component, private StreamHost,
                         private DeliveryHost, private SessionHost, private LinkTimingHost
    {
    public:
      static constexpr size_t MAC_ADDR_STR_LEN = 18; // "AA:BB:CC:DD:EE:FF\0"

      // A listener that goes away must not leave the onboarding gate held by a
      // pointer nobody will ever release (a host test builds and drops many).
      ~LORAListener();

      void setup() override;
      void dump_config() override;

      void set_parent(LORATracker *parent) { parent_ = parent; }
      void set_short_address(uint8_t address) { this->short_address_ = address; }
      // The address MAC-0 routes this node's uplinks by (UplinkDemux.h).
      uint8_t short_address() const { return static_cast<uint8_t>(this->short_address_); }
      void set_subnet_address(uint8_t address) { this->subnet_address_ = address; }
      // Seconds, 60..604800 (the YAML schema's range = the node's clamp).
      void set_sleep_duration(uint32_t sleep_duration) { this->sleep_duration_ = sleep_duration; }
      void set_battery_update_interval(uint32_t seconds) { this->battery_update_interval_ = seconds; }
      void enterSleep();
      void triggerOTA();
      // Runtime, on-demand debug toggles (DebugFlags.h on the node side,
      // 2026-09-29). One thin wrapper for all three -- unlike enterSleep()/
      // triggerOTA() these carry no node-side precondition the hub needs to
      // reason about, so there is nothing here worth a dedicated method per
      // op. `op` is a raw ClientOperation value (CLIENT_OPERATION__CMD_DEBUG_*)
      // rather than the enum type itself, for the same reason send_tracked_
      // sysop_() below takes int32_t: this header deliberately does not
      // include blinds.pb-c.h (see the forward-declare note near the top).
      void sendDebugSysop(int32_t op);
      virtual void set_response(uint8_t *data, size_t len);

      void send_login() { this->session_.send_login(); }
      // P1: push wall-clock time to the node.  The node has no clock source of
      // its own, so this is its only way to learn the time.  Sent once the
      // encrypted session is confirmed (see set_response), because before that
      // point the node may not be able to authenticate the frame.  A no-op when
      // the hub's own clock is not yet valid — sending epoch 0 would be worse
      // than sending nothing, and the node ignores it anyway.
      void send_timesync();

      // ---- P4: schedule push ----
      // The hub owns the schedule; the node holds a working copy. They are
      // reconciled by VERSION: the node echoes the version it has applied in
      // every beacon, and the hub pushes whenever that differs from its own.
      // A node asleep right now therefore picks up an edit at its next wake.
      struct SchedEntryCfg
      {
        uint16_t minute_of_day;
        uint8_t  day_mask;      // bit0 = MON .. bit6 = SUN
        uint8_t  action;        // SchedAction
        uint8_t  position_pct;
        bool     enabled;       // a slot can be switched off without losing it
      };
      static constexpr uint8_t kMaxScheduleEntries = 8;

      // Persisted slot table. Versioned like LORAClientRestoreState so a layout
      // change discards the old blob instead of misreading it.
      // v2 added auto_mode. Bumping discards any v1 blob, which costs nothing:
      // v1 was never deployed.
      static constexpr uint8_t kSchedPersistVersion = 2;
      struct SchedPersist
      {
        uint8_t       version;
        uint8_t       count;
        // The user's INTENT, not the switch's displayed state.
        //
        // The beacon handler corrects the switch to whatever the node actually
        // reports; auto_mode_ is what was asked for. Persisting the switch
        // would persist the display — a value reality overwrites — so intent is
        // what belongs here.
        //
        // It lives in this blob rather than in a separate preference because it
        // already feeds schedule_version(): it is part of the same config unit,
        // and one blob makes it impossible for the schedule to survive a reboot
        // while the mode that executes it does not.
        bool          auto_mode;
        SchedEntryCfg entries[kMaxScheduleEntries];
      } __attribute__((packed));

      void set_auto_mode_default(bool on) { this->auto_mode_ = on; this->sched_dirty_ = true; }
      void set_interactive_timeout(uint32_t s) { this->interactive_timeout_ = s; this->sched_dirty_ = true; }
      void set_checkin_interval(uint32_t s)    { this->checkin_interval_ = s;    this->sched_dirty_ = true; }
      void set_beacon_lead(uint32_t s)         { this->beacon_lead_ = s;         this->sched_dirty_ = true; }
      void set_post_event_window(uint32_t s)   { this->post_event_window_ = s;   this->sched_dirty_ = true; }
      void set_catchup_window(uint32_t s)      { this->catchup_window_ = s;      this->sched_dirty_ = true; }
      // ---- Home Assistant schedule editing -------------------------------
      //
      // The YAML schedule is a FIRST-BOOT SEED. Once a slot has been persisted
      // (by an HA edit) the stored value wins, so adding this feature does not
      // silently discard a schedule that is already in service.
      //
      // Every setter marks sched_dirty_, which recomputes schedule_version();
      // the node then sees a version mismatch in its next beacon and the hub
      // pushes. No protocol change — the same path a YAML edit already took.
      //
      // A sleeping node does not learn of an edit until it next wakes, up to
      // checkin_interval_. The `Schedule Pending` binary sensor already shows
      // exactly that window.
      // ---- compact text form ---------------------------------------------
      //
      // Applies a whole schedule from one string (see ScheduleText.h). Returns
      // false and fills `err` when the text is rejected — in which case NOTHING
      // is applied, because a partially applied schedule is a schedule nobody
      // asked for.
      bool set_schedule_text(const char *text, char *err, size_t err_len);
      // The canonical rendering of what is actually stored, for read-back.
      void schedule_text(char *out, size_t out_len) const;

      void save_schedule_();
      void restore_schedule_();

      void add_schedule_entry(uint16_t minute_of_day, uint8_t day_mask,
                              uint8_t action, uint8_t position_pct);
      // Set the node's mode at runtime (HA switch). Marks the schedule dirty so
      // the change reaches the node on its next beacon.
      void set_auto_mode(bool on);
      bool get_auto_mode() const { return this->auto_mode_; }
      // CRC32 of the canonical schedule blob — what the node echoes back.
      uint32_t schedule_version();
      // True when the node's applied version differs from ours.
      bool schedule_pending();
      void send_schedule_config();

      // --- Drift test (bench only) --------------------------------------
      // Puts BOTH ends into test mode: the node holds continuous RX, and the
      // hub emits a burst every grid_ms so there is a regular ruler to measure
      // against. Without the hub half the node would only get a fit whenever
      // the hub happened to have traffic, which in interactive mode is rare
      // and irregular.
      //
      // The grid frame IS the DriftTest message, re-sent. That makes the test
      // self-limiting: each one re-arms the node deadline, so if the hub stops
      // or this component reboots, the node exits on its own.
      void start_drift_test(uint32_t duration_s = 300, uint32_t grid_ms = 15000)
      {
        this->drift_.start(duration_s, grid_ms);
      }
      void stop_drift_test() { this->drift_.stop(); }
      bool drift_test_active() const { return this->drift_.active(); }

      // ModeTest — the on-hardware validation mode (test-plan.md section 10).
      //
      // Same shape as the drift test, and deliberately so: the two lessons that
      // shape drift_timer_cb_ were paid for on hardware and apply unchanged
      // here. The pre-built frame goes through the NORMAL transmit queue, never
      // sendPacketOnce() from a timer callback — that bypasses TxDone handling
      // and the return-to-RX in the tracker loop, and left the SX1278 stuck in
      // TX with the hub silent until restarted (2026-08-31). And the NEXT frame
      // is built immediately after each send, in the idle 99% of the period, so
      // packing cannot leak into the interval being measured.
      void start_mode_test(uint32_t duration_s, uint32_t grid_ms, uint32_t mode,
                           uint32_t copies, bool keep_power_profile,
                           bool enable_counter, bool enable_crypto,
                           bool mac_echo, int32_t arm_offset_us = 0)
      {
        if (this->parent_ == nullptr)
          return;
        this->mbs_owns_test_ = this->mbs_starting_;
        this->mode_test_.start(duration_s, grid_ms, mode, copies, keep_power_profile,
                               enable_counter, enable_crypto, mac_echo, arm_offset_us);
      }
      void stop_mode_test() { this->mode_test_.stop(); }
      bool mode_test_active() const { return this->mode_test_.active(); }
      // U-5: whether this node has missed a check-in it was predicted to make.
      // Published as a diagnostic because that is what an operator acts on; it
      // also gates single-shot through HubBelief::beacon_missed.
      bool node_overdue() const { return this->beacon_overdue_(); }

      // How long past a predicted check-in the node is given before it counts
      // as missed. Longer than its whole beacon ladder, which is what has to
      // complete before a missed check-in is real: the node beacons within
      // seconds of waking and re-beacons twice before its resume fallback
      // fires, and automode::kQuietWindowMinMs (17 s) is sized to cover that
      // entire sequence. 60 s is comfortably past it, and the node's clock
      // comes FROM this hub so there is no skew term to budget for.
      //
      // Public for the same reason kUplinkOffsetUs is: it is a protocol
      // constant, and a test that restated it could not fail when it changed.
      static constexpr uint32_t kBeaconOverdueGraceS = 60;

      // For tests. Production sets this from the hub clock when a beacon
      // arrives (handle_beacon_); a test needs to place it independently of
      // `now` to exercise the comparison, which is the whole of U-5.
      void noteBeaconEpochForTest(uint32_t epoch) {
        this->last_beacon_epoch_ = epoch;
      }

      // For tests: how many times this listener's session keys have been
      // (re)derived — mirrors CmdDispatcher::sessionGenerationForTest().
      // For tests: the node's MAC-1 counters (read-only).
      const mac1::Sequencer &sequencer() const { return this->session_.seq(); }
      uint32_t sessionGenerationForTest() const { return this->session_.generation(); }
      // For tests: install a session directly (sets the base nonce AND
      // derives K_enc/K_mac from it), without a real LOGIN/uplink round
      // trip. Mirrors CmdDispatcher's setBaseNonceForTest reasoning: some
      // tests need "a second, DIFFERENT session now exists" as a precise,
      // one-line precondition rather than a second full handshake.
      bool deriveSessionKeysForTest(uint32_t session_id, uint32_t node_nonce)
      {
        return this->session_.derive_keys_for_test(session_id, node_nonce);
      }
      // For tests: the 16-byte K_root every K_auth/K_enc/K_mac is derived from
      // (SHA-256(fleet key phrase)[0:16]). A wrong byte here is invisible to
      // anything that derives and compares on the SAME side, so a test pins it.
      static bool deriveRootKeyForTest(uint8_t out[16]);
      // For tests: the msgid / CTR-space guard (see HubSession::request_msgid_relogin).
      bool msgidReloginPendingForTest() const { return this->session_.msgid_relogin_pending(); }
      uint32_t msgidReloginRequestsForTest() const { return this->session_.msgid_relogin_requests(); }
      void clearMsgidReloginForTest() { this->session_.clear_msgid_relogin(); }
      // For tests: encrypt a block under this listener's CURRENT K_enc with
      // CTR — the only way to prove the hub and node landed on bit-IDENTICAL
      // session keys, since opaque PSA key ids can't be compared directly.
      bool ctrEncryptForTest(const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
      {
        return this->session_.ctr_encrypt_for_test(iv, in, len, out);
      }

      // U-2: the node's radio-busy skip count, as of its last PhaseReport.
      //
      // This is the one Mode B failure the KPIs cannot see. A window the node
      // never armed is invisible to WMR by construction — noteMarkArmed() is
      // what opens a mark — so a node whose radio is wedged reports a perfect
      // window-mark rate while hearing nothing at all. Publish it beside WMR,
      // never instead of it: a rising count here is what says a perfect WMR is
      // measuring an empty denominator.
      //
      // `valid` means A REPORT HAS ARRIVED, exactly as ModeTestSummary::valid
      // does — not "this firmware has the field". It cannot mean that:
      // rxBusySkips is a proto3 scalar, so absent and 0 are the same bytes.
      // The submessage around it is detectable (notePhaseReport_ takes nullptr
      // for a node that sends no PhaseReport at all) but a scalar inside it is
      // not.
      //
      // That distinction is moot here rather than papered over: nothing is
      // deployed, so no firmware predating this field will ever report to this
      // hub. If any ever did, it would publish 0 and read as a clean radio —
      // and the fix then is the node's fwVersion, which the hub already tracks,
      // not a companion bool in this message.
      struct RxBusySkips { bool valid{false}; uint32_t count{0}; };
      RxBusySkips node_rx_busy_skips() const {
        return RxBusySkips{this->link_.have_rx_busy_skips(), this->link_.node_rx_busy_skips()};
      }

      // The report the node sent back, verbatim. Empty until one arrives.
      //
      // Kept, but it is not the entity to publish: Home Assistant caps a
      // text_sensor state at 255 characters and this line is up to 512, so a
      // single sensor carrying it would silently truncate — the same trap the
      // `help` entity fell into. The NUMBERS below are what belongs in HA, one
      // per sensor, because B3's gate ("reception >= Mode A over a week") is a
      // week of history and a string cannot be graphed.

      // --- The last ModeTest report, as numbers ---------------------------
      //
      // Recomputed by the hub from the node's RAW counters (I1: the node never
      // grades itself), stored so HA can plot them rather than have an operator
      // read them out of an ESPHome log line.
      using ModeTestSummary = lora_tracker::ModeTestSummary;   // BenchStreams.h
      const ModeTestSummary &mode_test_summary() const { return this->mode_test_.summary(); }

      // --- MAC-0 ping / echo (mac-layer.md sections 5 and 6) --------------
      //
      // The hub half of the MAC control frame. Emits a MacControl PING on a
      // grid and records the echoes the node's MAC-0 sends back, WITHOUT any
      // application message in either direction. That is what makes the frame
      // funnel and the turnaround measurable at all: every existing exchange
      // has a cover operation or a schedule somewhere inside it.
      //
      // Single copy per mark, expressible only since the per-frame TxPolicy
      // replaced the global setBurstCopies. A burst would defeat the purpose —
      // seventeen copies of one mark is not one mark.
      // The stream itself is MacPingStream (BenchStreams.h); these forward.
      void start_mac_ping(uint32_t duration_s = 300, uint32_t grid_ms = 1100,
                          bool want_echo = true, uint32_t pad_bytes = 0,
                          bool crypto = false)
      {
        this->mac_ping_.start(duration_s, grid_ms, want_echo, pad_bytes, crypto);
      }
      void stop_mac_ping() { this->mac_ping_.stop(); }
      bool mac_ping_active() const { return this->mac_ping_.active(); }
      using MacStats = MacPingStream::Stats;
      // --- B1: grid-aligned downlink -------------------------------------
      //
      // DEFAULT OFF, and that is the whole point of shipping it this way.
      //
      // Aligning a burst to the node's slot costs up to one round (1.5 s) of
      // added latency on a user command, and buys NOTHING until the node
      // actually opens a single window at that instant — which is B3. A node in
      // Mode A listens three windows every 1.5 s regardless of where the burst
      // starts, so turning this on now would be a pure regression in
      // responsiveness. B3 enables it per node as those nodes are promoted.
      // --- B3: publish / withdraw the grid --------------------------------
      //
      // send_grid_sync(false) is the STARTUP BROADCAST DEMOTE, and it must go
      // out before anything else. The anchor is "set once and never moved", so
      // after a hub restart the hub holds a fresh anchor while every node still
      // holds the old one. Without this each node would burn up to resyncMaxS
      // (11.7 min) of missed windows before demoting itself — and the beacon
      // that would re-anchor it is on a grid it no longer shares.
      void send_grid_sync(bool enable);
      // WHY a placed Class A copy did or did not go out.
      //
      // This was a bool, and the bool collapsed two decline reasons that want
      // OPPOSITE answers from the caller. A node the hub cannot PLACE — not
      // Class A, or never heard from — still wants the burst, because the hub
      // does not know where it listens and 17 copies span every window it
      // might open. A Class A node whose RX1 and RX2 have BOTH closed is
      // asleep, and the hub knows it exactly: a burst aimed there is 1408 ms
      // of air with nothing listening for it, colliding with every other
      // node's window for no chance of being heard.
      //
      // Numbered for the log line, not published to Home Assistant.
      using ClassAPlacement = lora_tracker::ClassAPlacement;   // NodeLinkTiming.h

      // Place one copy in this node's next OPEN Class A window — RX1 if the
      // queue can still fire on it, otherwise RX2, which the node arms exactly
      // when RX1 passed with no data.
      ClassAPlacement send_into_class_a_window_(const uint8_t *buf, size_t len,
                                                const mac2::FrameSeal &seal = {})
      {
        return this->link_.send_into_class_a_window(buf, len, seal);
      }
      // The mark a placed frame for this node would go out on. Shared with
      // send_grid_sync, which must declare the round it will be transmitted in.
      // The frame's SHAPE is required, not optional: clearance depends on how
      // long the transmission occupies the air, and burstEndUs_ derives that
      // from copies x stride + time-on-air(len) + an optional response window.
      // Without it this asked the two-argument question, which places after the
      // END of the latest reservation and so skips marks whose air was free.
      int64_t nextPlacementT0_(int64_t now_us, int copies, size_t len,
                               bool expects_reply) const
      {
        return this->link_.next_placement_t0(now_us, copies, len, expects_reply);
      }
      // Capture this node's own uplink T0 and score it against the grid. Called
      // only for a frame that has earned the replay counter — see
      // commit_rx_msgid_.
      // MAC-0 receive ticket (S5, mirrors the node's RxTicket/commitTiming): what
      // MAC-0 knows about the frame being processed -- the tracker's T0 stamp,
      // its uncertainty and the on-air length (always a trustworthy T0 input in
      // v1). Captured at admission (set_response),
      // committed by MAC-1 only once the frame has earned it (commit_rx_msgid_).
      struct Mac0Rx
      {
        mac::RxTicket ticket{};
        uint32_t      unc_us{0};
      };
      Mac0Rx mac0_ticket_(size_t len) const;
      // THE MAC-0 timing commit: this node's uplink origin (C2 Class A windows,
      // §4.6 placement belief) moves here and nowhere else.
      void   mac0_commit_timing_(const Mac0Rx &rx);
      Mac0Rx rx_ticket_{};
      // §4.6's promotion evidence, taken from a DECRYPTED uplink — the wake
      // beacon or, far more usefully, a MAC ACK.
      void notePhaseReport_(const ::PhaseReport *pr) { this->link_.note_phase_report(pr); }

     public:
      // For tests: the same entry point both carriers use, so a test of the
      // POLICY transitions does not have to mint an encrypted frame each time.
      // Two tests drive the real encrypted paths end to end, which is what pins
      // that handle_beacon_ and handle_command_ack_ actually call this.
      // Defined in the .cpp: building a ::PhaseReport needs the generated
      // struct, and this header only forward-declares it.
      void notePhaseReportForTest(uint32_t rtc_slow_src, int32_t err_us,
                                  int32_t spread_us, uint32_t samples,
                                  uint32_t outside_guard = 0,
                                  bool node_timed_rx = true);

      // §4.6: did this uplink land where the grid says this node transmits?
      // Called with the node's own T0, from admit_frame_.
      // belief_ with confirmation_age_s filled in from the clock — the one
      // field that is a function of now rather than of an event.
      timedmode::HubBelief hubBeliefNow_() const { return this->link_.belief_now(); }
      // Broadcast withdrawal, addressed to every node at once. Deliberately
      // separate: after a restart the hub may not yet know which nodes exist.
      void broadcast_grid_demote();

      // B3: put this node into (or out of) Mode B. THE ENTRY POINT THAT WAS
      // MISSING — `send_grid_sync(true)` was never called from anywhere and
      // `set_grid_aligned()` had no callers, so no node could ever be told to
      // adopt a grid and the hub's own alignment could not be switched on.
      // Mode B was unreachable from the hub side however correct the node was.
      //
      // Deliberately an explicit opt-in, defaulting OFF, exposed as a switch
      // rather than done automatically: `T_detect` is unmeasured (HW-2), so the
      // guard band Mode B rests on is an assumption, and §4.7 notes that
      // turning alignment on costs up to 1.5 s of latency and buys nothing
      // until the timed window exists. Promotion is per node, as §8's B3 row
      // has always said.
      //
      // Also what unblocks HW-2: the sweep needs a GridSync carrying
      // `armOffsetUs`, and until now the hub emitted none.
      void enable_timed_mode(bool on);
      bool timed_mode_enabled() const { return this->timed_mode_enabled_; }

      // OPTIMISTIC SINGLE SHOT — opt-in, defaulting OFF, exposed as a switch
      // for the same reason enable_timed_mode is: it relaxes §4.6's "any hub
      // uncertainty -> burst", which is the safety argument.
      //
      // It lets the hub spend ONE placed copy instead of a 17-copy burst when
      // the only rung refusing single shot is that its confirmation has aged
      // out. Rule 4 bounds a wrong guess to one frame (an unacked single shot
      // is retried as a burst immediately). Break-even is p = 6 %; a node
      // genuinely in Mode B measured 198/198 and 194/194 windows hit.
      //
      // An OPERATOR PREFERENCE, not an observation — which is why it is a
      // member here and stamped into the struct by hubBeliefNow_(), rather
      // than a field of belief_. belief_ carries only what the hub has
      // observed; a setter on it would be exactly the "claim" §4.6 rejects.
      void enable_optimistic_single_shot(bool on);
      bool optimistic_single_shot() const { return this->optimistic_single_shot_; }
      // §4.6's hub belief, as the policy sees it. Read-only: every field is
      // maintained from an observed event, and a setter would be the "claim"
      // the whole section exists to reject.
      timedmode::HubBelief hubBelief() const { return this->hubBeliefNow_(); }
      // What this listener would do RIGHT NOW — the production decision itself,
      // not a re-derivation of it. send_aligned_ asks this, and so do the
      // tests, so a test cannot pass a staleness bound the hub never published.
      timedmode::TxPolicy txPolicyNow() const { return this->link_.tx_policy_now(); }
      // U-1: WHY this listener is still bursting, from the same ladder the
      // decision above comes out of — not a re-derivation of it.
      //
      // The boolean alone was never the diagnostic. A node stuck on bursts is
      // paying seventeen copies for every frame, which is the whole cost Mode B
      // exists to remove, and "single shot: no" gives an operator nothing to
      // act on. Which of the fourteen conditions failed does.
      timedmode::TxRefusal txRefusalNow() const {
        return timedmode::txRefusalFor(this->hubBeliefNow_(), timedgrid::kGuardUs,
                                       this->published_resync_max_s_);
      }
      // The node replies at its mark + this rather than "immediately"; must be
      // >= the measured DRAIN + build time, which is HW-7's number. Published
      // to the node in GridSync.ulOffsetUs and used by the hub to decide
      // whether an uplink arrived in slot, so the two must be the same number —
      // it lives on the class rather than in the .cpp for that reason.
      static constexpr uint32_t kUplinkOffsetUs = NodeLinkTiming::kUplinkOffsetUs;

      void set_grid_aligned(bool v) { this->grid_aligned_ = v; }
      bool grid_aligned() const     { return this->grid_aligned_; }
      uint8_t grid_slot() const     { return this->grid_slot_; }

      // SHADOW pending-data mask (PendingShadow.h): everything the would-be
      // mask decision reads about THIS node, gathered in one place. Called from
      // LORATracker::serviceBeacon only, never from the transmit path. Not
      // const because schedule_version() lazily recomputes its CRC.
      pendingshadow::NodeInputs pending_shadow_inputs(int64_t now_us);
      // For tests: optimistic-single-shot bookkeeping of the tracked command.
      bool op_short_wait_spent_for_test() const { return this->ctl().short_wait_spent; }
      bool op_sent_optimistic_for_test() const  { return this->ctl().sent_optimistic; }
      singleshotwait::AckLatency &ack_latency_for_test() { return this->link_.ack_latency(); }
      // For tests: the node's link timing (belief, placement, uplink stamp).
      NodeLinkTiming &link_timing() { return this->link_; }
      uint32_t op_first_tail_ms_for_test() const { return this->ctl().first_tail_ms; }

      // Test seam: send_aligned_ is protected, and the alignment POLICY is the
      // part worth testing directly rather than only through a full command.
      // (a'): the sleep model's answer, for tests.
      uint32_t ms_until_node_awake_for_test() const { return ms_until_node_awake_(); }
      // D4: fire one ModeTest mark exactly as the esp_timer would.
      void mode_test_tick_for_test() { this->mode_test_.tick_for_test(); }
      void drift_tick_for_test() { this->drift_.tick_for_test(); }
      std::vector<uint8_t> mode_test_frame_for_test() const {
        return this->mode_test_.frame_for_test();
      }
      void send_aligned_for_test(const uint8_t *buf, size_t len) {
        this->send_aligned_(buf, len);
      }
      // The onboarding gate is process-wide, so a test must be able to start
      // from a free one and see who holds it.
      static void reset_onboarding_gate_for_test() { HubSession::reset_onboarding_gate_for_test(); }
      static void reset_sleep_gate_for_test() { s_sleep_gate_.reset(); }
      bool onboarding_held_for_test() const { return this->session_.onboarding_held(); }
      int64_t last_timesync_sent_us_for_test() const { return this->last_timesync_sent_us_; }
      // Simulates the config-confirmed-and-saved moment without driving a full
      // REGISTER/LOGIN exchange, for the reboot-persistence tests.
      void save_state_for_test() { this->save_state_(true); }
      // Fire one MAC ping exactly as the esp_timer would.
      void mac_ping_tick_for_test() { this->mac_ping_.tick_for_test(); }
      // What confirm_session_ sets, without its pushes: for a test that needs a
      // confirmed session and will drive the follow-up traffic itself.
      void mark_session_confirmed_for_test() {
        this->session_.login_state_for_test().confirmed       = true;
        this->session_.login_state_for_test().relogin_pending = false;
      }
      // The session's state machine, read-only (HubSession::LoginState).
      bool     session_confirmed() const { return this->session_.confirmed(); }
      bool     login_acked() const { return this->session_.login_acked(); }
      bool     relogin_pending() const { return this->session_.relogin_pending(); }
      uint32_t pending_login_nonce() const { return this->session_.pending_login_nonce(); }
      uint8_t  login_retry_count() const { return this->session_.login_retry_count(); }
      bool     startup_login_initiated() const { return this->session_.startup_login_initiated(); }
      // For tests: set a precondition of the session's state machine.
      HubSession::LoginState &login_state_for_test() { return this->session_.login_state_for_test(); }
      // The Control channel's command, read-only.
      const ControlCommand &ctl_cmd() const { return this->ctl_cmd_; }
      // For tests: set up a channel's delivery state directly.
      DeliveryService::SlotState &dlv_slot_for_test(DeliveryChannel ch)
      {
        return this->dlv_.slot_for_test(ch);
      }

      // --- Tracked-delivery state, read-only -------------------------------
      //
      // What the end-to-end suite asserts a conversation against, and what a
      // diagnostic could publish: is this listener still waiting for an ack,
      // did it give up, and is a schedule push outstanding. All three were
      // observable only through a log line or a binary sensor.
      bool     awaitingAck() const     { return this->ctl().awaiting_ack; }
      bool     commandFailed() const   { return this->command_failed_; }
      // For tests: whether the tracked-op retry buffer (frame, the ONE
      // place a sealed ciphertext is stored and re-sent later) is empty —
      // invariant I3 (a) needs this cleared on every fresh send_login()
      // challenge, see clearSessionKeys_().
      bool     trackedOpFrameEmptyForTest() const { return this->ctl().frame.bytes.empty(); }
      // The msgid of the latest ScheduleConfig attempt, 0 when none is on record
      // (acked, or never sent). Kept after the ladder is spent, so a late ack is
      // still recognised.
      uint32_t schedulePushMsgid() const { return this->cfg().acks.last(); }
      // A published GridSync the node has not confirmed yet, and how many times
      // it has been re-published for that.
      bool     gridSyncAwaitingAck() const { return this->mac().awaiting_ack; }
      uint8_t  gridSyncRepublishes() const { return this->gridsync_republishes_; }
      // Node-initiated GridSync requests, answered and not (hubAnswersSyncRequest).
      uint32_t gridSyncRequestsAnswered() const { return this->gridsync_requests_answered_; }
      uint32_t gridSyncRequestsIgnored() const  { return this->gridsync_requests_ignored_; }
      // Plaintext uplinks refused because this node holds a confirmed session.
      uint32_t plaintextRefused() const { return this->plaintext_refused_; }
      uint32_t opRetryCount() const    { return this->ctl().retry_count; }
      const MacStats &mac_stats() const { return this->mac_ping_.stats(); }
      void reset_mac_stats() { this->mac_ping_.reset_stats(); }

      // --- Mode C's Class A ledger ------------------------------------------
      //
      // The machinery to measure Class A reception already existed on both
      // ends and was never added up. The hub places one reply into RX1 on
      // every check-in (send_timesync -> send_into_class_a_window_), and the
      // node reports that wake's funnel in its NEXT beacon
      // (prevwakewindows/hits/detected/crcvalid, "Mode C's FER" in its own
      // comment). But the two lived in a log line and a beacon field, so a
      // success rate had to be correlated by hand — and an empty window is
      // ambiguous between "nothing was aimed at it" and "aimed and missed".
      //
      // RAW COUNTERS, not a computed rate: the same rule as MacStats and the
      // ModeTest report. The node never grades itself; the hub adds up what
      // each end actually observed and the operator divides.
      //
      //   hit rate  = hits / offered
      //   true FER  = 1 - crcValid / detected     (stage 2->3, mac-layer 6.1)
      //
      // offered is counted at the PLACEMENT, so it answers "did the hub aim a
      // frame at a window that was open", which is the question an empty
      // window could not previously distinguish.
      struct ClassAStats
      {
        uint32_t offered   = 0;  // replies the hub PLACED in an open window
        uint32_t windows   = 0;  // windows the node reports opening
        uint32_t hits      = 0;  // windows that caught something
        uint32_t detected  = 0;  // frames the modem detected
        uint32_t crc_valid = 0;  // of those, CRC-valid
      };
      const ClassAStats &class_a_stats() const { return this->class_a_stats_; }

     protected:
      // block == nullptr: a fresh FCNT (loop only); else the run's reserved
      // block (any task). See pack_stream_frame.
      void handle_mode_test_report_(const ::ModeTestReport *rep) { this->mode_test_.on_report(rep); }

      // Same shape as the drift frame, and for the same reason: the NEXT frame
      // is packed immediately after a send, in the 99 % of the period that is
      // idle, so packing never lands inside the interval being measured.
      // Transmit `buf` either now or at this client's next grid T0, according
      // to grid_aligned_. Takes a COPY when it defers: every caller frees its
      // buffer immediately after handing it over.
      // Two overloads rather than a default argument: TxPolicy is only
      // forward-declared here, and `= {}` needs the complete type.
      // Both return whether the frame ENTERED THE TRANSMIT QUEUE. They used to
      // return void, and that was the hole T-2 turned out to be about: the
      // mark-consumption was handled correctly inside, but no producer could
      // see a refusal, so a frame lost to an exhausted buffer pool was lost
      // full stop. It matters most for the frames that carry no ack and have
      // no retry (a key-installing frame was the worst case, before the node
      // dropped BaseNonceExchange; LOGIN now carries the nonce with its own retry).
      // `seal`: which keys the frame's copies are tagged under (a plaintext frame
      // passes none). A sealed frame sent without its seal is dropped per copy.
      bool send_aligned_(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal = {})
      {
        return this->link_.send_aligned(buf, len, seal);
      }
      bool send_aligned_(const uint8_t *buf, size_t len, const TxPolicy &policy,
                         const mac2::FrameSeal &seal = {})
      {
        return this->link_.send_aligned(buf, len, policy, seal);
      }
      // LinkTimingHost (lora_client_timing.cpp).
      const char *link_name() const override;
      bool link_has_radio() const override;
      bool link_grid_aligned() const override;
      bool link_grid_started() const override;
      uint32_t link_slot() const override;
      int64_t link_next_t0(int64_t now_us) const override;
      int64_t link_next_clear_t0(int64_t now_us, int copies, size_t len,
                                 bool expects_reply) const override;
      bool link_send(const uint8_t *buf, size_t len, const TxPolicy &policy,
                     const mac2::FrameSeal &seal) override;
      bool link_timed_mode_enabled() const override;
      bool link_optimistic_single_shot() const override;
      uint32_t link_resync_max_s() const override;
      bool link_beacon_overdue() const override;
      bool link_firmware_known() const override;
      uint32_t link_node_mode() const override;
      void link_class_a_offered() override;

      // ---- S4: the single MAC data path (docs/mac-separation-layering-plan.md)
      // One packed hub->node data frame, ready for MAC-0.
      using MacFrame = DeliveryFrame;
      // THE framing path for hub->node data frames: the header (this node, the
      // hub, the msgid), the pack and, under a confirmed session, the seal.
      // `op` carries the payload only. The first form allocates the msgid from
      // the session counter and encrypts iff the session is confirmed; the
      // second is the MAC ping's (reserved / dedicated msgids, its own choice).
      bool mac_frame_(hubframes::Down *op, MacFrame *out, bool ack_req = false);
      bool mac_frame_(hubframes::Down *op, uint32_t msgid, bool encrypt, MacFrame *out,
                      bool ack_req = false);
      // THE MAC-0 data request: every frame this listener puts on the air
      // passes here, with its copy sealer attached when it is sealed.
      bool mac0_send_(const uint8_t *buf, size_t len, TxPolicy policy, const mac2::FrameSeal &seal);

      // B4: the exact bytes of the tracked op in flight, so a retry
      // retransmits them rather than re-packing from live state.

      // StreamHost: what the bench streams may ask of the listener
      // (lora_client_bench.cpp).
      const char *stream_name() const override;
      bool stream_grid_started() const override;
      int64_t stream_next_t0(int64_t now_us) const override;
      int64_t stream_next_clear_t0(int64_t now_us, int copies, size_t len,
                                   bool expects_reply) const override;
      bool stream_has_radio() const override;
      bool stream_frame_fresh(hubframes::Down *op, DeliveryFrame *out) override;
      bool stream_send(const uint8_t *buf, size_t len, const TxPolicy &policy,
                       const mac2::FrameSeal &seal) override;
      bool stream_session_confirmed() const override;
      bool stream_grid_ready() const override;
      bool stream_frame(hubframes::Down *op, uint32_t fcnt, bool encrypt, DeliveryFrame *out) override;
      bool stream_reserve_fcnts(uint32_t n, mac1::FcntReservation *r) override;
      uint32_t stream_take_fcnt(mac1::FcntReservation *r) override;
      bool stream_sealed_in_epoch(const DeliveryFrame &f, const mac1::FcntReservation &r) override;
      void stream_invalidate_fcnts(mac1::FcntReservation *r) override;
      bool stream_send_placed(const uint8_t *buf, size_t len, const TxPolicy &policy,
                              const mac2::FrameSeal &seal, int64_t *last_mark_t0_us) override;
      void stream_set_timeout(const char *name, uint32_t ms, std::function<void()> fn) override;
      void stream_cancel_timeout(const char *name) override;

      // Login stagger slot: 0, 1, 2 ... in YAML declaration order.
      //
      // NOT derived from short_address_. That was the original scheme
      // (address x 3000 ms) and with addresses 17 and 18 it produced 51 s and
      // 54 s of dead air after every hub restart -- the SEPARATION was the
      // intended 3 s, but the OFFSET scaled with an arbitrary address value.
      //
      // Nor address % N: that is a hash, and hashes collide. Addresses 17 and
      // 25 would share a slot and transmit on top of each other -- trading a
      // predictable delay for an intermittent collision, which is worse
      // because it only appears once enough nodes exist.
      //
      // A sequential slot is collision-free by construction and costs
      // (node_count - 1) x 3 s at worst, which is the minimum any
      // non-overlapping schedule can achieve.
      // When the node was last heard from (its wake beacon).
      //
      // Used as the reference for the periodic check-in when last_sleep_epoch_
      // is unavailable. That field is only set when the HUB puts the node to
      // sleep, and an automatic-mode node sleeps on its OWN initiative — so it
      // stays 0 forever, next_wake_epoch_() returns 0, is_node_awake_() falls
      // back to "cannot tell — assume awake", and the hub fires 17-copy login
      // bursts at a node that is provably asleep. Observed 2026-08-31: five
      // retries at node 2 while its own log read "sleeping 1390 s until next
      // scheduled wake".
      //
      // A beacon is the right reference because the node sends one on every
      // wake and sleeps a few seconds later, so beacon + checkin_interval is
      // the next check-in to within the length of a wake.
      //
      // NOT NodeWakeBeacon.nextEventEpoch: that is the next SCHEDULED event
      // only. Using it would have the hub defer for hours while the node was
      // actually waking each check-in interval.
      uint32_t last_beacon_epoch_{0};

      uint8_t  login_slot_{0};
      static uint8_t s_next_login_slot_;
     public:
      // Tests: a fresh boot's slot numbering (setup() claims slots in order).
      static void resetLoginSlotsForTest() { s_next_login_slot_ = 0; }
     protected:

      // TimeSyncPolicy: when this node last received one, esp_timer time.
      // 0 = never (the sentinel every other "last X" field in this class uses).
      int64_t  last_timesync_sent_us_{0};
      // The onboarding gate: HubSession's (see HubSession.h).


      // Grid slot, claimed in declaration order exactly as login_slot_ is, so
      // it is stable across reboots without any extra configuration.
      uint8_t  grid_slot_{0};
      bool     grid_aligned_{false};
      // B3 opt-in; see enable_timed_mode().
      bool     timed_mode_enabled_{false};
      // Optimistic single shot opt-in; see enable_optimistic_single_shot().
      // Deliberately NOT a field of belief_: that struct holds only observed
      // evidence, and this is an operator's choice. hubBeliefNow_() stamps it.
      bool     optimistic_single_shot_{false};

      // §4.6's hub half. TimedModePolicy.h defines HubBelief and txPolicyFor()
      // and had no production caller at all: the rule "send ONE copy once the
      // node's uplinks are observed in their slot" was written, tested and
      // never asked. The whole airtime saving of Mode B is in that sentence —
      // 17 copies to 1 — so leaving it unasked meant Mode B cost the same as
      // Mode A and bought only a narrower window.
      //
      // Every field is maintained from a real event; nothing here is a claim
      // the node makes about itself, which is the distinction §4.6 turns on.
      // The node's link timing (NodeLinkTiming.h): §4.6's belief and evidence,
      // the placement memory, this node's uplink stamp and ack latency.
      NodeLinkTiming link_{*this};
      // The resyncMaxS this hub actually published, and the bound on how stale
      // a phase report may be before single-shot is withdrawn. Held rather than
      // recomputed so the two can never disagree: the node demotes itself on
      // the value it was SENT, and a hub trusting a report older than that
      // would keep sending single copies to a node already back on a
      // free-running window. Zero until a grid is published, which refuses
      // promotion — the safe direction.
      uint32_t published_resync_max_s_{0};

      // The drift test (BenchStreams.h): its state, timer and FCNT block.
      DriftStream drift_{*this};

      // ModeTest (BenchStreams.h). Every frame carries the full request (the
      // node arms on the first one it hears, so a node that missed the first is
      // not left waiting for a repeat that never comes).
      ModeTestStream mode_test_{*this};

      // MAC-0 ping / echo (BenchStreams.h): its state, timer and FCNT block.
      MacPingStream mac_ping_{*this};
      // Mode C's Class A ledger; see class_a_stats().
      ClassAStats class_a_stats_{};
      // The prevbeaconfcnt whose funnel has already been added. Each beacon
      // reports the PREVIOUS wake, so two beacons from one wake would count
      // that wake twice — exactly what fw 1.1.2 and 1.1.3 did before the
      // one-uplink fix. Keying on the msgid makes the accumulation idempotent
      // rather than trusting that fix to hold.
      uint32_t    class_a_last_prev_msgid_{0};

      // N-3 (hub side): the last resetReason this listener has already acted
      // on, so a run of beacons from the SAME boot (every Mode-C wake reports
      // the same value) only advances tx_message_id once per actual reboot,
      // not once per beacon. 0xFFFFFFFF never matches a real IDF reset-reason
      // code, so the very first beacon always counts as "new".
      uint32_t    last_handled_reset_reason_{0xFFFFFFFFu};

     public:

      // ---- P2: wake beacon ----
      // Latest values reported by the node, for logging and HA diagnostics.
      // clock_offset_s_ is node_epoch - hub_epoch: positive means the node runs
      // ahead. This is the measurement the no-sleep-cap decision rests on.
      bool     clock_offset_valid_{false};
      int32_t  clock_offset_s_{0};
      uint32_t last_beacon_reason_{0};
      uint32_t node_mode_{0};
      // handle_register_ defers a provisioned node's config push to
      // confirm_session_, the first moment it can be encrypted.
      bool                 config_push_pending_{false};
      // ---- the acked config push (review rollout-3 D1) ----
      // While collecting, send_remote_config / send_cover_config queue ACKED
      // requests for the setup channel instead of firing; they go one at a time,
      // and the node is recorded in sync (config_synced_, the persisted hash)
      // only when every one was acked.
      bool                 setup_collect_{false};
      bool                 setup_failed_{false};
      std::deque<DeliveryRequest> setup_queue_;
      // The config hash the node last ACKNOWLEDGED (restored, or set when a push
      // is acked): what is persisted while the current config is not in sync.
      uint32_t             synced_config_hash_{0};
      // The CURRENT config was acknowledged by the node (restored hash match, or
      // an acked push this boot): only then is the current hash persisted.
      bool                 config_hash_acked_{false};
      void push_config_acked_(bool notify_children);
      void start_setup_();
      bool config_goes_acked_() const;
      bool setup_in_progress_() const
      {
        return !this->setup_queue_.empty() || this->dlv_.slot(CH_SETUP).awaiting_ack;
      }
      void enqueue_setup_(DeliveryRequest req);
      void setup_next_();
      DeliveryRequest acked_setup_request_(const char *what,
                                           std::function<void(hubframes::Down *)> fill);
      // A REGISTER whose children notice (on_register) waits for the session.
      bool pending_register_notice_{false};
      // Duplicate suppression for PLAINTEXT uplinks before a session exists
      // (accept_plaintext_uplink_, review HB4). Separate from the MAC-1 rx
      // mark, which an unauthenticated frame never moves; never persisted;
      // reset by every LOGIN and kept with the counter by commit_rx_msgid_.
      uint32_t plaintext_hwm_{0};
      bool     have_plaintext_hwm_{false};
      uint32_t node_sched_version_{0};
      uint32_t node_fw_version_{0};
      bool     node_session_resume_{false};

      // P4 schedule state. sched_version_ is cached and recomputed lazily
      // whenever sched_dirty_ is set, so an HA edit never has to remember to
      // invalidate it by hand.
      bool          auto_mode_{false};
      uint32_t      interactive_timeout_{1800};
      uint32_t      checkin_interval_{21600};
      uint32_t      beacon_lead_{30};
      uint32_t      post_event_window_{20};
      uint32_t      catchup_window_{1800};
      SchedEntryCfg sched_entries_[kMaxScheduleEntries]{};
      uint8_t       sched_entry_count_{0};
      // Set once a persisted schedule has been restored, so the YAML seed
      // stops overwriting it.
      bool          sched_restored_{false};
      ESPPreferenceObject sched_pref_;
      uint32_t      sched_version_{0};
      bool          sched_dirty_{true};
      // A schedule push is a SINGLE unbursted frame (it is too long for the
      // 88 ms burst slot), so a lost one used to be lost for good: no schedule
      // means the node never enters auto mode, never sleeps, never wakes, never
      // beacons — and the beacon is what would have triggered a re-push.
      // Retransmit until the node's MAC ACK confirms delivery.
      // The push is the Config channel's request (dlv_.slot(CH_CONFIG), deliver_).
      // A GridSync is re-published until the node's MAC ACK names one of its
      // msgids. Measured 2026-09-15 on node 2: the single GridSync sent after a
      // login was lost (the node spent the burst in CAD retries) and nothing
      // re-sent it — 13 minutes in Mode A with Timed Mode ON. Exact msgids, not a
      // range: a tracked op sent in between must not be mistaken for it.
      // It is the Mac channel's request (dlv_.slot(CH_MAC), deliver_): the ids are
      // its ack-id SET, the ladder is deliverypolicy::kGridSyncPolicy.
      static constexpr uint8_t  kGridSyncMaxRepublishes = deliverypolicy::kGridSyncPolicy.max_retries;
      static constexpr uint32_t kGridSyncRetryMs        = deliverypolicy::kGridSyncPolicy.base_wait_ms;
      // Re-publishes of the latest ladder (kept after the ack, for the log and tests).
      uint8_t       gridsync_republishes_{0};
      // When a GridSync was last published (esp_timer us, 0 = never), for the
      // once-a-minute bound on answering a node's request.
      int64_t       last_gridsync_publish_us_{0};
      // When the node last ACKNOWLEDGED a GridSync (esp_timer us, 0 = since the last
      // session never). ModeBSupervisor settles on it; see mbs_grid_acked_().
      int64_t       last_gridsync_ack_us_{0};
      // A re-publish after a node reboot / NoGrid refusal is scheduled and not yet sent.
      bool          regrid_pending_{false};
      // When we last advanced tx_message_id for a node boot (0 = never) and for which
      // wake (the beacon's prevBeaconMsgId); when the last reboot-driven re-publish
      // was decided; when a spent ladder last forced a LOGIN.
      uint32_t      last_wake_key_{0};
      int64_t       last_regrid_us_{0};
      int64_t       last_forced_relogin_us_{0};
      // Any ack from the node since the session was last confirmed; and whether the
      // post-sleep login fallback has already looked once more.
      bool          acked_since_confirm_{false};
      bool          fallback_rechecked_{false};
      void advance_tx_for_boot_(const ::NodeWakeBeacon *b);
      void post_sleep_fallback_fire_();
      uint32_t s_since_(int64_t t_us) const;

      // --- ModeBSupervisor glue (ModeBSupervisor.h) ------------------------
      // Keeps a node whose Timed Mode switch is ON in Mode B across node sleep,
      // node reboot and hub reboot, by running a short ModeTest B warm-up
      // whenever the node is logged in but not promoted.
      modebsup::Supervisor mbs_;
      bool          mbs_owns_test_{false};   // the ModeTest on the air is the supervisor's
      bool          mbs_starting_{false};    // set only while the supervisor calls start_mode_test
      ModeTestSummary mbs_summary_before_{};   // the last report held when the warm-up started
      uint32_t      mbs_warmups_started_{0}; // diagnostic: warm-ups begun since boot
      // ONE token hub-wide: shared by every listener.
      static modebsup::Arbiter &mbs_arbiter_();
      static void resetModeBArbiterForTest() { mbs_arbiter_().reset(); }
      uint32_t mbs_id_() const { return this->short_address_; }
      uint32_t mbs_now_s_() const;
      void start_mode_b_supervisor_();
      void mbs_tick_();
      bool mbs_supervised_() const;
      bool mbs_other_test_() const;
      modebsup::Inputs mbs_inputs_() const;
      void mbs_apply_(modebsup::Event ev);
      void mbs_start_warmup_();
      void mbs_abort_warmup_(bool send_stop);
      void mbs_send_stop_frame_();
      void mbs_yield_to_op_();
      void mbs_yield_self_();
      void mbs_restart_();
      bool mbs_grid_acked_() const;
      bool mbs_report_fresh_() const;
      void restore_timed_mode_at_boot_();
      void stop_mode_test_(bool send_stop) { this->mode_test_.stop_(send_stop); }
      const modebsup::Supervisor &modeBSupervisor() const { return this->mbs_; }
      uint32_t      gridsync_requests_answered_{0};
      uint32_t      gridsync_requests_ignored_{0};
      uint32_t      plaintext_refused_{0};
      static constexpr uint8_t  kSchedMaxRetries   = deliverypolicy::kSchedulePolicy.max_retries;
      static constexpr uint32_t kSchedRetryMs      = deliverypolicy::kSchedulePolicy.base_wait_ms;
      static constexpr uint32_t kMaxAckBacklogMs   = DeliveryService::kMaxAckBacklogMs;
      // `base_ms` from NOW is when an ack should have arrived if the frame just
      // handed to the tracker were on the air already. It is not: it waits behind
      // whatever is queued, so the timer is the queue's drain time PLUS the base.
      // Call it after send(), so the frame just sent is counted as well.
      uint32_t ack_wait_ms_(uint32_t base_ms) const { return this->dlv_.ack_wait_ms(base_ms); }
      virtual void send_remote_config();
      uint32_t incrTxMessageId() { return this->session_.allocate_tx(); }
      void setRxMessageId(uint32_t msg_id) { this->session_.commit_rx(msg_id); }

      // The ONE way a child cover hands its CoverConfig to the radio: a Fire request
      // through the delivery mechanism, so it is encrypted whenever the session is
      // confirmed and plaintext only before that — packing raw and calling
      // parent_->send() is what sent a CoverConfig in the clear onto a confirmed
      // session, which the node refuses. Header and msgid are the mechanism's.
      // Returns false when it could not be packed or the transmit pool refused.
      bool send_cover_config(const ::CoverConfig &cc);

      // F-4: Send a cover operation with delivery tracking.  The operation is
      // stored so it can be retransmitted (with a fresh, incrementing msgid)
      // until the node returns a MAC ACK.  After kOpMaxRetries the command is
      // marked failed and surfaced to Home Assistant via the optional binary
      // sensor.  covop_case / operation use the generated LoraCoverOperation /
      // CovOperation enum values; position is used only for the POSITION case.
      //
      // Returns false when the request was IGNORED because the hub models the node
      // as asleep (node_asleep_()): nothing was sent or queued, no failure raised.
      // The caller (the HA cover) must then not claim the move happened.
      bool send_cover_operation(uint32_t covop_case, int32_t operation, float position);
      void set_command_failed_binary_sensor(binary_sensor::BinarySensor *bs) { this->command_failed_bsensor_ = bs; }
      // P4b: HA control/visibility of automatic mode.
      void set_auto_mode_switch(switch_::Switch *sw) { this->auto_mode_switch_ = sw; }
      void set_schedule_pending_binary_sensor(binary_sensor::BinarySensor *bs) { this->schedule_pending_bsensor_ = bs; }
      // Diagnostics of the hub's sleep model (SleepModel.h): "Asleep" is ON from
      // the SLEEP ack until the node is awake again; "Sleep Status" says why.
      void set_asleep_binary_sensor(binary_sensor::BinarySensor *bs) { this->asleep_bsensor_ = bs; }
      void set_sleep_status_text_sensor(text_sensor::TextSensor *ts) { this->sleep_status_tsensor_ = ts; }


      virtual void set_address(uint64_t address)
      {
        this->address_uint64_ = address;
        this->remote_bda_[5] = (address >> 40) & 0xFF;
        this->remote_bda_[4] = (address >> 32) & 0xFF;
        this->remote_bda_[3] = (address >> 24) & 0xFF;
        this->remote_bda_[2] = (address >> 16) & 0xFF;
        this->remote_bda_[1] = (address >> 8) & 0xFF;
        this->remote_bda_[0] = (address >> 0) & 0xFF;


        if (address == 0)
        {
          this->address_str_[0] = '\0';
        }
        else
        {
          format_mac_addr_upper(this->remote_bda_, this->address_str_);
        }
      }
      const char *address_str() const { return this->address_str_; }

      void set_time(time::RealTimeClock *time) { this->time = time; }

      void register_lora_node(LORAClientNode *node)
      {
        // node->client = this;
        node->set_lora_client_parent(this);
        this->nodes_.push_back(node);
      }

    protected:
      std::vector<LORAClientNode *> nodes_;

    public:
      LORATracker *parent_{nullptr};

    protected:
      // Was uninitialized (same latent-UB pattern as registered_'s old
      // declaration below) — a LORAListener constructed without set_time()
      // read this as a wild non-null pointer and setup() dereferenced it
      // (now()/add_on_time_sync_callback()), crashing once session_tx_mutex_
      // shifted the object's layout enough to turn the garbage value into an
      // unmapped address. Every production lora_client instance always sets
      // time_id, so this never fired on hardware, but nullptr is the only
      // safe default for a pointer nothing has set yet.
      time::RealTimeClock *time{nullptr};

      // Group 1: 8-byte types
      uint64_t address_uint64_{0};

    public:
      // For tests (S4): retag one stamped copy exactly as the tracker does for a
      // frame, with the copy sealer bound to the LAST frame this listener sealed
      // (production never calls this: the tracker uses each frame's own sealer,
      // TxPolicy::sealer). Writes enc->tag in place; false = the copy is dropped
      // (no session, the keys changed since that frame was sealed, a copy for
      // another node, or a tag buffer of the wrong size).
      // For tests: how many burst copies of this node's frames were refused as
      // stale (sealed under keys the node no longer holds).
      uint32_t staleSessionDropsForTest() const { return this->session_.stale_drops(); }

      uint8_t  short_address_{0};
      uint8_t  subnet_address_{0};
      uint64_t sleep_duration_{86400}; // Default to 24 hours
      uint32_t battery_update_interval_{900}; // Battery force-send interval [s]; default 15 min
      // Was uninitialized (relying on incidental zero memory to read as
      // false) — found when adding session_tx_mutex_ shifted this object's
      // layout and three MacPing tests started reading registered_ as true
      // garbage. A node is not registered until handle_register_ sets this.
      bool     registered_{false};
      static constexpr int64_t kRegisterRateLimitUs = HubSession::kRegisterRateLimitUs;

      // MAC-1 (stage S3): HubSession owns the node's Sequencer (HubSession.h).
      void set_counters_no_write_(uint32_t tx, uint32_t rx) { this->session_.set_counters_no_write(tx, rx); }
      // ---- the MAC-1 counter interface ------------------------------------
      // The only operations the application uses to move the counters (the
      // allocation and the commit are incrTxMessageId / setRxMessageId);
      // tools/layer_lint.py counts every other writer. None writes NVS.
      // A new session (or no persisted state): both directions restart at 0.
      void mac1_restart_session_() { this->set_counters_no_write_(0, 0); }
      // Install a persisted (tx, rx) snapshot.
      void mac1_restore_(uint32_t tx, uint32_t rx) { this->set_counters_no_write_(tx, rx); }
      // FCNT blocks for the esp_timer streams: HubSession's (see HubSession.h).
      bool reserve_fcnt_block_(uint32_t n, mac1::FcntReservation *r)
      {
        return this->session_.reserve_fcnts(n, r);
      }
      uint32_t take_reserved_fcnt_(mac1::FcntReservation *r) { return this->session_.take_fcnt(r); }
      // Every counter restart / key change drops every block (loop).
      void invalidate_fcnt_reservations_();
      bool sealed_in_epoch_(const MacFrame &f, const mac1::FcntReservation &r)
      {
        return this->session_.sealed_in_epoch(f, r);
      }
      // Placement for a frame built on the esp_timer task with an explicit
      // shape (MAC ping): touches no loop-owned state (last_placed_*, belief).
      bool send_placed_from_timer_(const uint8_t *buf, size_t len, TxPolicy policy,
                                   const mac2::FrameSeal &seal, int64_t *last_mark_t0_us);
      // Spin until an esp_timer callback that may still be running has
      // returned (its timer is already stopped and its active flag false).
    public:
      // For tests: put the counters anywhere (a boundary, an exhausted space).
      void setCountersForTest(uint32_t tx, uint32_t rx) { this->set_counters_no_write_(tx, rx); }

      // msgId of the frame link_.last_uplink_t0_us() belongs to.
      uint32_t last_uplink_msgid_{0};
      // Mode C, MAC-0: this node's wake-clock rate against the hub, from the
      // RTC tick counts its beacons report. See WakeClockFit.h.
      wakeclock::Fit wake_fit_{};

      char address_str_[MAC_ADDR_STR_LEN]{}; // 18 bytes: "AA:BB:CC:DD:EE:FF\0"

      // Login state — cleared on every hub reboot or enterSleep(); set when the
      // node sends a valid message after the login challenge.
      uint32_t last_sleep_epoch_{0};   // Unix epoch when enterSleep() was last called
      // (a'): Unix epoch of the last ACCEPTED uplink from this node — a REGISTER
      // for its MAC or an authenticated frame. See is_node_awake_().
      uint32_t last_heard_epoch_{0};
      // Same event on the monotonic clock (esp_timer), 0 = never. The shadow
      // pending mask needs "how long ago" without depending on a valid wall clock.
      int64_t  last_heard_us_{0};
      void     note_node_heard_();
      // The login / session state machine (login_acked, session confirmed,
      // relogin pending, the pending base nonce, retries): HubSession's.
      // This node's session keys, generation and installed session id live in
      // mac2::SessionKeyStore (HubLinkSecurity.h) since stage S2 — the single
      // owner, keyed by short_address_. The installed session id is what lets
      // handle_encrypted_ tell "this session-opening uplink is for the session
      // I already hold" (a duplicate burst copy must not force a fresh PSA
      // import and bump the generation) apart from "this is a NEW session".
      // True once this hub boot has pushed configuration to the node.  Reset to
      // false on every hub reboot (member default), so a config change flashed
      // into the hub is delivered exactly once per node per boot; routine wakes
      // of an already-synced, provisioned node skip the config bursts entirely.
      bool     config_synced_{false};

      static constexpr uint8_t  kRestoreStateVersion  = 3;   // +config_hash
      static constexpr uint8_t  kMaxLoginRetries        = HubSession::kMaxLoginRetries;
      static constexpr uint32_t kLoginRetryIntervalMs   = HubSession::kLoginRetryIntervalMs;
      static constexpr uint32_t kLoginRetryBaseMs       = HubSession::kLoginRetryBaseMs;
      static constexpr uint32_t kRegisterToLoginDelayMs = HubSession::kRegisterToLoginDelayMs;
      static constexpr uint32_t kRegisterToLoginFastMs  = HubSession::kRegisterToLoginFastMs;

    protected:
      esp_bd_addr_t remote_bda_; // 6 bytes

      optional<LORAClientRestoreState> restore_state_();
      void save_state_(bool save);
      // FNV-1a over the fields ClientConfig actually carries. Pure and
      // deterministic — same inputs, same hash, on both sides of a reboot.
      uint32_t currentConfigHash_() const;

      // P2: record a wake beacon and publish the derived clock offset.
      void handle_beacon_(const ::NodeWakeBeacon *b);
      // Phases of handle_beacon_ (it was CCN 32).
      void note_wake_clock_(const ::NodeWakeBeacon *b);
      void note_class_a_ledger_(const ::NodeWakeBeacon *b);
      bool note_node_reset_(const ::NodeWakeBeacon *b);   // true: the reset reason is new
      void note_beacon_clock_(const ::NodeWakeBeacon *b);
      void sync_auto_mode_switch_(const ::NodeWakeBeacon *b);
      // A node that lost its RAM grid (reboot on a resumed session, or a warm-up it
      // refused for want of one): forget what we believed and publish it again.
      // See modebsup::beaconMeansGridLost.
      void regrid_after_reboot_(const ::NodeWakeBeacon *b, bool reset_changed);
      void note_warmup_refusal_();
      bool regrid_node_(const char *why);
      bool regrid_due_() const;
      void forget_node_grid_();
      void regrid_fire_();
      // A node asks for its grid again. See GridSyncRequest in blinds.proto.
      void handle_grid_sync_request_(const ::GridSyncRequest *r);

      // Phases of set_response(), which was a single 250-line body at CCN 61 —
      // the hub mirror of the node's onReceiveNew (70 -> 15). The ORDER in
      // set_response is load-bearing: REGISTER must bypass the address and
      // replay filters, so it is tested before admit_frame_().
      void handle_register_(hubframes::Up *rcv_message, uint8_t *data, size_t len)
      {
        this->session_.handle_register(rcv_message, data, len);
      }
      bool admit_frame_(hubframes::Up *rcv_message);
      void handle_encrypted_(hubframes::Up *rcv_message, uint8_t *data, size_t len);
      // The node's MAC-2 link (HubSession.h): keys, v1 framing and seal, the
      // uplink's security stages, the msgid guard.
      HubSession session_{*this};
      // The last stage of handle_encrypted_ (the frame is authentic and decrypted).
      void     uplink_accept_(EncUplink &up, uint8_t *plaintext);
      // ---- MAC wire v1 (S8, docs/mac-wire-format.md): HubSession's, forwarded ----
      bool v1_frame_(hubframes::Down *op, uint32_t fcnt, bool encrypt, MacFrame *out)
      {
        return this->session_.frame(op, fcnt, encrypt, out);
      }
      // The v1 LOGIN path of send_login (frame, send, log).
      // The REGISTER's authentication: the K_AUTH tag over the raw frame.
      // The v1 shape check + decoded view (set_response's first step).
      std::unique_ptr<hubwire::Uplink> unpack_uplink_(uint8_t *data, size_t len);
      bool v1_login_frame_(const ::LoginMsg &login, uint32_t fcnt, MacFrame *out)
      {
        return this->session_.login_frame(login, fcnt, out);
      }
      bool v1_demote_frame_(MacFrame *out) { return this->session_.demote_frame(out); }
      void v1_deliver_decrypted_(EncUplink &up, const uint8_t *pt, size_t len);

      // Two stages of set_response(): the length re-encode check, and the
      // plaintext-uplink gate (true = the frame may be acted on).
      bool accept_plaintext_uplink_(hubframes::Up *rcv_message);
      // HubSession's key lifecycle (see HubSession.h for the rules).
      void clearSessionKeys_() { this->session_.clear_keys(); }
      // SessionHost (lora_client_session.cpp).
      const char *session_name() const override;
      uint8_t session_peer() const override;
      uint8_t session_subnet() const override;
      uint32_t session_cmd_generation() const override;
      void session_keys_installed() override;
      void session_keys_cleared() override;
      void session_relogin() override;
      void session_set_timeout(const char *name, uint32_t ms, std::function<void()> fn) override;
      void session_save_state() override;
      void session_cancel_timeout(const char *name) override;
      bool session_has_radio() const override;
      uint64_t session_mac_address() const override;
      uint32_t session_login_slot() const override;
      uint32_t session_ms_until_awake() const override;
      uint32_t session_ack_wait_ms(uint32_t base_ms) const override;
      bool session_send(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal) override;
      int64_t session_rx_t0_us() const override;
      bool session_send_on_register_mark(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal,
                                         int64_t t0_uplink) override;
      int64_t session_send_burst_at(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal,
                                    int64_t phase_fire_us) override;
      bool session_send_after_uplink(const uint8_t *buf, size_t len, const mac2::FrameSeal &seal,
                                     int64_t t0_uplink) override;
      void session_register_accepted() override;
      bool session_register_config(bool needs_config) override;
      void session_challenge_started() override;
      void session_counters_restarted() override;
      bool session_request_register() const override;
      void session_on_confirmed() override;
      void session_on_login_acknowledged() override;
      void session_node_restored(uint32_t fcnt_jump) override;
      int64_t session_tx_drain_us() const override;
      bool session_gridsync_outstanding() const override;
      // Move the replay counter. Called only for a frame that has been
      // authenticated, or one that arrived before any session existed.
      void commit_rx_msgid_(const hubframes::Up *rcv_message);
      void dispatch_payload_(hubframes::Up *msg);
      void dispatch_mac_payload_(hubframes::Up *msg);
      // S6: the listener's own handling (dispatch_payload_, demultiplexed by
      // PortTable.h), then -- for a mac::Port::App frame only -- one decoded
      // AppIndication to each child. `outer` is the frame's (outer) header.
      void deliver_uplink_(hubframes::Up *msg, const hubframes::Hdr *outer, bool authentic);
      void confirm_session_() { this->session_.confirm(); }
      void send_deferred_tracked_op_();
      void send_deferred_config_push_();
      void schedule_config_sync_relogin_();
      bool more_traffic_owed_();
      bool timesync_due_();

      // Login scheduling helpers
      // Epoch of the node's next wake, or 0 if the hub cannot tell.
      // Understands BOTH sleeps: the interactive fixed sleep_duration_, and
      // automatic mode's schedule-derived wake. See the definition.
      uint32_t next_wake_epoch_() const;

      // U-5: has this node failed to appear when it said it would?
      //
      // §11b called this "a feature rather than a fix" — the hub having to
      // compare a predicted check-in against an observed one. It turned out
      // the hub already HAS the prediction: next_wake_epoch_() above, computed
      // from the node's own vendored scheduler for a different purpose (not
      // transmitting at a sleeping node). Only the comparison was missing.
      //
      // BOTH clauses are needed:
      //   * the predicted wake has passed by more than the grace, AND
      //   * nothing has been heard SINCE that predicted wake.
      // The second is not belt-and-braces. In automatic mode the prediction can
      // be pinned near `now` by an imminent scheduled event, so the first
      // clause alone would mark an awake, healthy, beaconing node overdue once
      // the grace elapsed.
      //
      // FAILS OPEN on a node it cannot predict (wake_at == 0: never heard
      // from, or no hub clock). A prediction the hub cannot make must never
      // deny single-shot permanently — that is precisely how the old in-slot
      // criterion became unsatisfiable.
      bool beacon_overdue_() const;


      bool     is_node_awake_() const;
      // The hub's sleep model (SleepModel.h): asleep / awake / unconfirmed / auto.
      sleepmodel::State sleep_state_() const;
      bool     node_asleep_() const { return this->sleep_state_() == sleepmodel::State::Asleep; }
      // Publish the "Asleep" / "Sleep Status" sensors if they changed and arm the
      // refresh that ends "asleep" at the modelled wake.
      void     refresh_sleep_status_();
      void     start_sleep_status_();
      void     publish_sleep_status_(sleepmodel::State s);
      // True (and logs one line) when a request must be ignored: the node sleeps.
      bool     refuse_while_asleep_(const deliverypolicy::ControlCmd &c);
      // Inputs of pending_shadow_inputs(), split out to keep it flat.
      bool     push_awaiting_ack_();
      bool     frames_queued_(int64_t now_us) const;
      uint32_t ms_until_node_awake_() const;
      void     schedule_startup_login_() { this->session_.schedule_startup_login(); }
      void     do_login_and_arm_retry_() { this->session_.do_login_and_arm_retry(); }

      // Msgid / CTR-space guard: HubSession::request_msgid_relogin.
      // Arm a ONE-SHOT login retry that re-arms itself.  Uses set_timeout rather than set_interval: an
      // interval's first firing is randomly phased within [0, interval), which
      // fired the "hourly" login retry within ~0.6 s of the challenge, stepping
      // on the node's burst-deferred login-ack and piling a redundant LoginMsg
      // burst on top of the register-triggered config bursts.
      void     login_retry_step_() { this->session_.login_retry_step(); }

      // ---- command ACK / retransmit state (cover ops AND sysops) ----
      // The Control channel of the delivery service carries both: the first
      // attempt is packed once and every retry re-sends the same bytes (same
      // FCNT, same cmd_id) until the node's MAC ACK, then the command is failed
      // (and, unless it was a SLEEP, the session rebuilt) after kOpMaxRetries.
      using TrackedOpKind = ControlCommand::Kind;
      // The delivery service (DeliveryService.h): the channels' slots, ack ids,
      // ladders and timers, read-only here. ctl() is the tracked cover-op /
      // sysop request, cfg() the schedule push, mac() the GridSync.
      DeliveryService dlv_{*this};
      const DeliveryService::SlotState &ctl() const { return this->dlv_.slot(CH_CONTROL); }
      const DeliveryService::SlotState &cfg() const { return this->dlv_.slot(CH_CONFIG); }
      const DeliveryService::SlotState &mac() const { return this->dlv_.slot(CH_MAC); }
      // The Control channel's command (what the request in flight IS).
      ControlCommand ctl_cmd_{};

      // ---- the delivery service, as the listener uses it ----
      void deliver_(DeliveryChannel ch, DeliveryRequest req) { this->dlv_.deliver(ch, std::move(req)); }
      bool dlv_ack_(uint32_t ack_msg_id) { return this->dlv_.ack(ack_msg_id); }
      void dlv_cancel_(DeliveryChannel ch) { this->dlv_.cancel(ch); }
      void dlv_forget_ids_() { this->dlv_.forget_ids(); }
      void arm_dlv_timer_(DeliveryChannel ch) { this->dlv_.arm_timer(ch); }
      // DeliveryHost (lora_client_delivery.cpp).
      const char *dlv_name() const override;
      bool dlv_has_radio() const override;
      bool dlv_relogin_pending() const override;
      bool dlv_frame(hubframes::Down *op, DeliveryFrame *out, bool ack_req) override;
      bool dlv_place_fire(deliverypolicy::Shape shape, const DeliveryFrame &f,
                          const char *what) override;
      void dlv_yield_warmup() override;
      int64_t dlv_tx_drain_us() const override;
      int64_t dlv_last_placed_t0_us() const override;
      singleshotwait::AckLatency &dlv_ack_latency() override;
      bool dlv_single_shot_unacked() const override;
      void dlv_mark_single_shot_unacked() override;
      void dlv_drop_session() override;
      void dlv_relogin() override;
      void dlv_set_timeout(const char *name, uint32_t ms, std::function<void()> fn) override;
      void dlv_cancel_timeout(const char *name) override;
      // Put the packed frame on the air the way `shape` says. True when it went out.
      bool     fire_place_(deliverypolicy::Shape shape, const MacFrame &f, const char *what);
      bool     fire_class_a_(const MacFrame &f, const char *what);
      // TimeSync / ClientConfig, as Fire requests.
      void     fill_timesync_(hubframes::Down *op, ::TimeSync *ts);
      void     fill_client_config_(hubframes::Down *op, ::ClientConfig *cc);
      // ScheduleConfig, as a request.
      bool     encode_schedule_(bool retry, uint32_t *version, const DeliveryRequest::FrameFn &frame);
      void     place_schedule_(const MacFrame &f, const DeliveryAttempt &a);
      bool     on_schedule_acked_(uint32_t version);
      void     on_schedule_failed_();
      bool     sched_push_outstanding_() const;
      // GridSync (enable), as a request. send_grid_sync(false) is NOT: a withdrawal
      // is a plain burst, never awaited or re-published.
      bool     encode_grid_sync_(bool retry, const DeliveryRequest::FrameFn &frame);
      void     place_grid_sync_(const MacFrame &f);
      int64_t  gridsync_planned_t0_{0};   // encode_grid_sync_ -> place_grid_sync_
      bool     on_grid_sync_acked_(uint32_t msgid);
      void     on_grid_sync_failed_();
      void     withdraw_grid_sync_();
      int64_t  grid_sync_planned_t0_();
      void     fill_grid_sync_(::GridSync *gs, int64_t planned_t0);
      // Something is being delivered to this node that a Mode B warm-up must not
      // run into: a cover op / sysop, a ScheduleConfig, a GridSync.
      bool     delivery_busy_() const
      {
        return this->ctl().awaiting_ack || this->ctl_cmd_.deferred_until_login ||
               this->cfg().awaiting_ack || this->mac().awaiting_ack ||
               this->setup_in_progress_();
      }
      bool     command_failed_{false};
      binary_sensor::BinarySensor *command_failed_bsensor_{nullptr};
      switch_::Switch             *auto_mode_switch_{nullptr};
      binary_sensor::BinarySensor *schedule_pending_bsensor_{nullptr};
      binary_sensor::BinarySensor *asleep_bsensor_{nullptr};
      text_sensor::TextSensor     *sleep_status_tsensor_{nullptr};
      // What was last PUBLISHED (publish on change only). -1 / empty: nothing yet.
      int8_t                       sleep_pub_asleep_{-1};
      std::string                  sleep_pub_text_;

      // 3000 ms (not 2000): a burst is ~1.5 s and the tracker holds a post-burst
      // RX window after it, so the cycle is ~1.9 s.  2000 ms re-burst almost
      // immediately, leaving no quiet gap for the node's deferred ACK — the
      // node's CAD saw the next burst as busy and backed off, so the ACK landed
      // a burst or two late.  3000 ms opens a ~1 s quiet listening gap where the
      // deferred ACK arrives and cancels the retransmit before it fires.
      static constexpr uint32_t kOpRetryIntervalMs = 3000;
      static constexpr uint8_t  kOpMaxRetries      = 4;
      // How long after the mark of a single placed tracked command its ack is
      // listened for (other nodes' bursts wait): the single-shot ack wait ceiling.
      static constexpr uint32_t kAckListenMs       = 3000;

      bool     encode_ctl_(const DeliveryRequest::FrameFn &frame);   // the cover/sys op's payload
      void     place_ctl_(const MacFrame &f, const DeliveryAttempt &a);
      // Send a tracked system operation (e.g. CMD_OTA) through the same
      // retransmit-until-acked path as cover ops.  sysop is a ClientOperation value.
      void     send_tracked_sysop_(int32_t sysop);
      // The supersession key/generation a tracked op's frame carries. One
      // place, because the first send and every retransmission must agree: a
      // retry under a fresh generation would supersede the frame it is a retry
      // of, which is the original bug with an extra step.
      //
      // Defined in the .cpp: TxPolicy is only forward-declared here (lora_client
      // is included BY lora_tracker.h, not the other way round), so an inline
      // body would need a complete type this header must not have.
      TxPolicy tracked_op_policy_() const;
      void     handle_command_ack_(uint32_t ack_msg_id, AckStatus status = ACK_STATUS__ACK_OK);
      // An ack whose status says the node will not execute the request (P3).
      bool     dlv_refused_(uint32_t ack_msg_id, AckStatus status)
      {
        return this->dlv_.refused(ack_msg_id, status);
      }
      // Stages of handle_command_ack_: each returns true when it consumed the ack.
      void     record_ack_latency_();
      void     set_command_failed_(bool failed);
      // ---- the Control channel's request (cover operations and sysops) ----
      // The command is the slot's own fields (kind, covop_case, ..., sysop); this
      // is the request that delivers it.
      DeliveryRequest ctl_request_();
      // Send the slot's command now (a fresh one, or the one a relogin deferred).
      void     ctl_send_();
      bool     on_ctl_acked_(uint32_t ack_msg_id);
      void     on_ctl_failed_();
      void     on_ctl_abandoned_();
      // A Control request from outside: starts now, replaces a cover op, or waits
      // its turn (DeliveryPolicy.h, ControlQueue).
      bool     ctl_submit_(const deliverypolicy::ControlCmd &c);
      // Make `c` the slot's command and send it, or leave it deferred for
      // confirm_session_() (`hold`: the node is asleep, not merely re-logging in).
      void     ctl_begin_(const deliverypolicy::ControlCmd &c, bool hold);
      void     ctl_enqueue_(const deliverypolicy::ControlCmd &c);
      // The command in flight is done (acked, failed or abandoned): start the next.
      void     ctl_next_();
      bool     ctl_busy_() const;
      // The commands waiting behind the one in flight.
      deliverypolicy::ControlQueue ctl_queue_;
      // The node acknowledged CMD_SLEEP and is asleep: what waited behind the sleep
      // is held for its wake (confirm_session_) instead of being sent into the void.
      bool     ctl_hold_for_wake_{false};
      // A sleep is in flight, deferred or waiting its turn.
      bool     sleep_pending_() const;
      // Drop a sleep that has waited too long for a session (in flight, or queued).
      bool     drop_stale_sleep_();

      // ---- Nightly sleep (enterSleep): a tracked sysop whose hub-side effects
      // are applied only when the node ACKNOWLEDGES it. See lora_client.cpp. ----
      // The tracked op in flight (awaiting its ack, or deferred behind a relogin)
      // is a CMD_SLEEP.
      bool     op_is_sleep_() const;
      bool     sleep_op_in_flight_() const;
      // Ack received: the node is going to sleep. Stamp + persist the sleep,
      // end the session on the hub side, arm the wake-time login fallback.
      void     on_sleep_acknowledged_();
      void     stamp_sleep_epoch_();
      void     arm_post_sleep_login_fallback_();
      // Retries exhausted without an ack: outcome unknown, keep everything.
      void     on_sleep_unconfirmed_();
      // A CMD_SLEEP stuck behind a relogin (deferred, or its retries deferred)
      // must not outlive the night: a sleep sent hours late is worse than none.
      void     arm_pending_sleep_expiry_();
      // ---- the nightly SLEEP, one node at a time (2026-11-01 23:00) ----
      // The hub cannot receive while it transmits. With every node's SLEEP on
      // the air at once, one node that does not answer (deaf, out of range)
      // makes the hub burst its retries exactly while the healthy nodes' acks
      // are due, and those acks are lost: every node ends "UNCONFIRMED". So a
      // node's SLEEP goes out only while it holds this process-wide gate (the
      // OnboardingGate class: one holder, FIFO), released when that SLEEP is
      // acked, unconfirmed, refused or dropped -- or at once when it did not go
      // on the air (deferred behind a relogin, or queued behind another command).
      static onboarding::Gate s_sleep_gate_;
      static constexpr uint32_t kSleepGatePollMs = 500;
      bool     sleep_waiting_{false};       // asked for, waiting for the gate
      bool     sleep_wait_logged_{false};
      void     try_start_sleep_();
      void     release_sleep_gate_() { s_sleep_gate_.release(this); }
      void     expire_pending_sleep_();
      static constexpr uint32_t kPendingSleepExpiryMs = 180000;

      ESPPreferenceObject rtc_;
      // The sleep record's commanded duration (review rollout-3 D2), in its own
      // preference so the restore state's layout (and kRestoreStateVersion) is
      // untouched. `epoch` ties it to the restore state's last_sleep_epoch.
      struct SleepRecord
      {
        uint32_t epoch;
        uint32_t duration_s;
      } __attribute__((packed));
      static constexpr uint32_t kSleepRecordVersion = 0x5EE90001u;
      ESPPreferenceObject sleep_pref_;
      // The sleep duration the node was given with the recorded SLEEP (0 =
      // unknown: a record from before this field; the current config is used).
      uint32_t last_sleep_duration_s_{0};
      SleepRecord saved_sleep_rec_{0, 0};   // what sleep_pref_ holds
      uint32_t interactive_wake_epoch_() const
      {
        return (this->last_sleep_epoch_ == 0) ? 0 : this->last_sleep_epoch_ + this->commanded_sleep_s_();
      }
      void restore_sleep_record_();
      void restore_config_sync_(uint32_t acked_hash);
      uint32_t commanded_sleep_s_() const
      {
        return this->last_sleep_duration_s_ != 0 ? this->last_sleep_duration_s_
                                                 : (uint32_t) this->sleep_duration_;
      }
    };

    inline void LORAClientRestoreState::apply(LORAListener *listener)
    {
      listener->mac1_restore_(this->tx_message_id + 64, this->rx_message_id);
      listener->registered_       = this->logged_in;
      listener->last_sleep_epoch_ = this->last_sleep_epoch;
    }

    class LORAClient : public LORAListener
    {
    public:
      // Memory optimized layout

    protected:
    };

  }
}