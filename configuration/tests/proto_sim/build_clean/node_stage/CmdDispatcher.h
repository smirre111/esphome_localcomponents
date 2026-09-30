#pragma once

#include <freertos/FreeRTOS.h>
#include <esp_pm.h>
#include <esp_timer.h>   // esp_timer_handle_t — P2b resume fallback
// F-25: <map> removed — std::map frame counters replaced by fixed PeerCounter array.
#include <queue>
#include "lora.h"
#include <iostream>
#include <sstream>
#include "MotorCtrl.h"
#include "SystemCtrl.h"
#include "LoraInterface.h"
#include "blinds.pb-c.h"
#include "psa/crypto.h"

#include "common.h"
#include "SessionManager.h"
#include "AutoModePolicy.h"
#include "DriftEstimator.h"
#include <string>
#include "sdkconfig.h"
#include "LoraTiming.h"
#include "MacFunnel.h"
#include "MacSublayers.h"
#include "PhaseTracker.h"
#include "TimedGrid.h"
#include "GridState.h"
#include "ModeTestPolicy.h"
#include "PendingData.h"
#include "AckCache.h"
#include "ClassAWindows.h"
#include "TimedModePolicy.h"
#include "FrameCrypto.h"

// F-39: Old Packet class definition removed 2026-05-25 — dead code.

class CmdDispatcher
{

// Configuration
// POOL_SIZE / BUFFER_SIZE come from common.h: LoraInterface hands us its
// buffer by pointer, so the two layers must agree. See the note there.
static const int POOL_SIZE = kLoraPoolSize;
static const int BUFFER_SIZE = kLoraBufferSize;
static const int RX_QUEUE_SIZE = 20;

public:
// Memory pool structure
typedef struct {
    uint8_t data[BUFFER_SIZE];
    size_t length;
    uint32_t timestamp;
    // DIO0 (RxDone) arrival instant in us, snapshotted from the ISR by
    // LoraInterface::onReceiveNew, which fills THIS struct (not its own pool —
    // that one is TX-side). Travels with the frame so the drift estimator sees
    // the interrupt time rather than the time the frame was decrypted.
    int64_t rx_us;
    // The receive window this frame actually arrived in, for the same reason
    // rx_us travels here: by the time the dispatcher resolves the frame, the
    // live generation has moved on. noteFrameArriving() bumps it BEFORE
    // lora_parsePacket (deliberately — see LoraInterface.cpp), and
    // noteRadioSlept() bumps it again, so a generation read at dispatch time is
    // at least two windows ahead of the one that caught this frame.
    //
    // Without it a mark could only be booked hit on "addressed to me", with no
    // way to ask WHICH window heard it — review finding 13, measured as
    // `armed 189, hit 189` at every sweep offset including both extremes.
    uint8_t window_gen;
} rx_buffer_t;

    CmdDispatcher(
        MotorCtrl *motCtrl,
        SystemCtrl *sysCtrl,
        LoraInterface *loraIf,
        portMUX_TYPE &motorMux,
        portMUX_TYPE &buttonMux);

    ~CmdDispatcher()

    {
    }

    enum SystemStatus : uint8_t
    {
        AVAILABLE,
        UNAVAILABLE,
        STATUS,
        UNKNOWN,
        ERROR
    };

    // F-4: TX command queue element.  `arg` carries per-command data atomically
    // with the command itself (currently the msgid to acknowledge for
    // SYSCMD_ACK), so processTxCommand no longer relies on a cross-task shared
    // slot that the dispatcher task could overwrite before the TX task read it.
    typedef struct {
        blinds_syscmd_base_t cmd;
        uint32_t arg;
    } tx_command_t;

    void setStatus(const uint8_t statusType, uint32_t arg = 0);
    void sendAvailable();
    void sendPosition();
    void sendBatteryVoltage();
    void setBatteryVoltage(float voltage);
    void measureAndSendBatteryVoltage();
    // LKG battery voltage cache -- the same value sendPosition()/sendBeacon()
    // read. Used by MotorCtrl to snapshot "voltage at move start" for the
    // battery-runtime calibration feature; whatever measureAndSendBatteryVoltage()
    // requested at the PREVIOUS move's end (or the periodic 5-min reading) is
    // what's fresh here -- a move's own fresh reading is not back yet by the
    // time it starts (it completes asynchronously on taskBatteryMonitor).
    float getLastBatteryVoltage() const;
    // F-41: taskBatteryMonitor raises this while it waits for the pack to
    // recover from a move and samples the ADC, so checkQueuesIdle() does not
    // let the node deep-sleep in the middle of a post-move battery update.
    void setBatteryMeasurementBusy(bool busy);
    void sendRegister();
    // The deep sleep about to be entered, as wanted and as handed to ESP-IDF
    // after the crystal correction. Kept across the sleep and reported in the
    // next wake's beacon (Mode C's wake-timing pass line).
    void noteDeepSleepRequest(uint64_t requested_us, uint64_t applied_us);

    // F-4: Acknowledge a received command (node -> hub) so the hub can stop
    // retransmitting.  Routed through processTxCommand (the single writer of
    // tx_message_id_) via SYSCMD_ACK; ack_msg_id travels with the queued command.
    void sendCommandAck(uint32_t ack_msg_id);

    // Burst scheduling: when a hub burst copy is received, onReceiveNew() records
    // the estimated end-of-burst timestamp (esp_timer_get_time() domain, us).
    // LoraInterface::loraRxTask reads this and defers the node's reply until the
    // burst is over, so the reply lands in the hub's post-burst RX window instead
    // of colliding with a burst copy.  0 = no active burst.
    int64_t getBurstEndUs() const { return this->burst_end_us_; }

    // F-5: Persist tx/rx message IDs and the hub base nonce to NVS so an
    // unexpected reboot can resume the encrypted session without a mandatory
    // re-login (and without the duplicate-msgid flood).  Login remains the
    // authoritative refresh path and overwrites this state when it occurs.
    void loadPersistentState();
    void savePersistentState();
    bool hasValidPersistentState() const { return session_.hasValidState(); }

    // Firmware version reported in the wake beacon, encoded as
    // major*10000 + minor*100 + patch (1.0.14 -> 10014).  A single integer the
    // hub can compare with >= to gate capabilities, without shipping a string.
    //
    // Derived from the build at RUNTIME, not hand-maintained.  It used to be a
    // constant with a "KEEP IN SYNC with PROJECT_VER" comment on it, and it did
    // not stay in sync: a node running 1.0.17 still announced 10014 in every
    // beacon.  A field whose whole purpose is to tell the hub which firmware a
    // node runs is worse than useless when it can quietly lie, and this project
    // has already lost time to not knowing what was actually on a node.
    static uint32_t firmwareVersion();

    // "1.0.17" -> 10017.  Split out from the esp_app_desc lookup so it can be
    // tested on the host; returns 0 for anything it cannot parse, which the hub
    // reads as "unknown" rather than as a misleading version.
    static uint32_t parseFirmwareVersion(const char *v);

    // ---- P2: wake beacon ----
    // Sent on every boot/wake. Carries the wake reason, the node's clock (so the
    // hub can measure drift), and whether the hub may skip the login handshake.
    static WakeReason classifyWakeReason();
    void sendWakeBeacon(WakeReason reason);
    // Why we are awake, remembered so the beacon can be sent at the right
    // moment rather than at boot.  See sendWakeBeacon()'s comment.
    void setWakeReason(WakeReason r) { this->wake_reason_ = r; }
    WakeReason getWakeReason() const { return this->wake_reason_; }

    // ---- P2b: resume-first wake ----
    // A provisioned node with a valid persisted session can skip the whole
    // REGISTER -> config -> login sequence (~4 s of awake radio) and just
    // announce itself with an encrypted beacon.  armResumeFallback() arms the
    // safety net: if no DECRYPTED downlink arrives in time, we re-register.
    // Without that net, a hub that rebooted while we slept would leave us
    // believing we are connected — a silent node, the worst failure here.
    // ---- Unprovisioned-node REGISTER retry ----
    // A node with no address (config address 0) is useless: it rejects the
    // hub's LoginMsg as "not for me", so the hub's request_register can never
    // reach it either.  The boot REGISTER used to be sent exactly ONCE, so a
    // single lost frame stranded the node permanently — recoverable only by a
    // physical reset.  Retry until the hub provisions us.
    //
    // The same holds for a PROVISIONED node without a session: sending
    // REGISTER drops the base nonce, so if that REGISTER is lost the node
    // cannot decrypt anything the hub sends. sendRegister() therefore always
    // arms the retry, and it stops only once the node has an address AND a
    // base nonce for its hub.
    bool isProvisioned();
    void armRegisterRetry();
    void cancelRegisterRetry();
    static constexpr uint32_t kRegisterRetryMs = 60000;

    // ---- clock retry (F14) ----
    // TimeSync is a single unbursted, unacknowledged frame, and the hub only
    // sends it on the login-ack and beacon paths. An AWAKE node does neither,
    // so losing that one frame left a node configured for auto mode sitting
    // interactive indefinitely with nothing to re-trigger it. Observed live:
    // CMD_LOGIN and CMD SCHEDULE both arrived, TimeSync did not, and the node
    // stayed awake for 25 minutes logging "the clock is not valid yet".
    //
    // Rather than retransmitting TimeSync blindly, the node asks again: the hub
    // already answers a wake beacon with a TimeSync, so re-sending the beacon
    // reuses that path. No proto change, and nothing is sent that the node does
    // not currently need.
    void armClockRetry();
    void cancelClockRetry();
    static constexpr uint32_t kClockRetryMs = 60000;

    bool canResumeSession();
    void armResumeFallback();
    void noteSessionProven();

    // --- MAC-0 (configuration/docs/mac-layer.md) --------------------------
    //
    // A MacControl frame is consumed HERE and never reaches an application
    // handler. No cover operation, no schedule, no motor — so there is nothing
    // to make inert, which is the whole reason the layer boundary is worth
    // having. Counters are raw: what the radio reported, not what the mode
    // believed.
    void handleMacControl(LoraClientOperationMessage *message_to_process,
                          const LoraHeader *outer_header, int64_t rx_us);
    void applyMacConfig_(const MacControl *mc);
    void handleGridSync(LoraClientOperationMessage *message_to_process,
                        const LoraHeader *outer_header, int64_t rx_us);
    static void sublayerRestoreCb_(void *arg);

    struct MacCounters
    {
        uint32_t ping_rx{0};    // MacControl PINGs addressed to us
        uint32_t echo_tx{0};    // echoes actually queued to the radio
        uint32_t echo_failed{0};
        uint32_t last_seq{0};
        int64_t  last_rx_us{0};      // RxDone timestamp of the last ping
        int64_t  last_echo_fire_us{0};
        // t_reply_fire - t_rxdone for the last ping, in microseconds. This is
        // the MAC turnaround, and the servable-slot geometry depends on it:
        // the ack case breaks above 36.5 ms against an assumed 20 ms that
        // budgets ZERO for the FIFO read, protobuf unpack and AEAD decrypt.
        int64_t  last_turnaround_us{0};
    };
    const MacCounters &macCounters() const { return this->mac_; }
    void resetMacCounters() { this->mac_ = MacCounters{}; }

    // M2 — the frame funnel (mac-layer.md section 6.1). Counted for EVERY
    // frame, not just MAC control frames: FER and WMR are properties of the
    // link, and a funnel that only saw test traffic would measure the test.
    const macfunnel::Counters &macFunnel() const { return this->funnel_; }
    // A CONSISTENT copy. macFunnel() hands out a reference into a struct two
    // cores write to, so a reader that walks its eight fields can see counts
    // from either side of an increment — and the ModeTest report walks all
    // eight and subtracts a baseline, where a single field from the wrong
    // moment turns into a negative delta clamped to zero. Anything reporting
    // the funnel takes the snapshot; tests reading one field may use the
    // reference.
    macfunnel::Counters macFunnelSnapshot()
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        const macfunnel::Counters c = this->funnel_;
        portEXIT_CRITICAL(&this->funnel_mux_);
        return c;
    }

    // The reset and the DIO0-task increments run on DIFFERENT CORES (the DIO0
    // task is pinned to core 1, the dispatcher to core 0). Per-field increments
    // are disjoint enough to be benign, but reset() assigns the WHOLE struct,
    // so a concurrent detected++ can be lost or can survive the reset — either
    // silently mixing two runs' KPIs.
    //
    // It IS symmetric now, and the way it is kept symmetric is that funnel_ is
    // private and every mutation goes through one of the wrappers below.
    //
    // It was not. The comment here used to claim "both sides take the same
    // spinlock" while six call sites in the RX dispatch path (noteMic,
    // noteAddressed, noteCounter, noteParsed) touched funnel_ directly with no
    // portENTER_CRITICAL, on taskDsptchLora (core 0), concurrently with
    // noteFrameDetected() on the DIO0 task (core 1). Non-atomic
    // read-modify-write on shared struct fields across cores: counts silently
    // lost, and a reset racing a dispatch leaving stale fields. It corrupts
    // KPIs rather than crashing, which is why it went unnoticed for as long as
    // it did — and why a comment asserting the invariant was worse than no
    // comment at all. See implementation-plan.md section 8, row M2.
    void resetMacFunnel()
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.reset();
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    // Called from the DIO0 task, which is where the CRC flag is known.
    void noteFrameDetected(bool crc_ok)
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteDetected(crc_ok);
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    void noteWindowResult(bool hit)
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteWindow(hit);
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    // The four RX-dispatch stages, each of which used to increment funnel_
    // straight from CmdDispatcher.cpp with no lock at all. They are wrappers
    // rather than a rule written in a comment, because the rule was written in
    // a comment and was false.
    void noteFrameParsed(bool ok)
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteParsed(ok);
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    void noteFrameAddressed(bool mine)
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteAddressed(mine);
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    void noteFrameCounter(bool accepted)
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteCounter(accepted);
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    void noteFrameMic(bool ok)
    {
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteMic(ok);
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    // WMR's two halves, reported from where each is actually known.
    //
    // Armed: by LoraInterface, at the moment a TIMED window opens. Free-running
    // Mode A windows are deliberately excluded — most of them are legitimately
    // empty because the hub is not sending, so counting them would make WMR a
    // measure of hub traffic rather than of whether marks are being met.
    // A mark, by contrast, is a promise.
    //
    // Hit: at the address filter, because a window that received a foreign
    // frame, a CRC failure or an unparseable one was NOT hit — the node spent
    // the battery and got nothing it could use.
    // A MARK BELONGS TO THE WINDOW IT WAS ARMED FOR (review finding 5,
    // 2026-09-15). The generation is LoraInterface's window counter, and it is
    // what stops two different windows resolving one mark:
    //
    //   * a mark window that heard a CRC failure or a NEIGHBOUR's frame left the
    //     mark open, because only an addressed frame closes it as a hit. The next
    //     window to close empty then booked the miss — a Mode A, beacon or Class A
    //     window that was never a mark, and a false step toward demotion.
    //   * that same mark was never counted missed either, so a node hearing its
    //     neighbour instead of its own frame every round never demoted, while the
    //     rule below says a miss is "no frame ADDRESSED to me at my mark".
    //
    // Arming the NEXT mark resolves the previous one: by then its round is over,
    // and the dispatcher task has had a full round to deliver a hit for it.
    // HW-2: a sweep deliberately mis-arms its windows, so the misses it causes
    // ARE the measurement, not evidence that the grid is stale.
    //
    // NO LONGER LOAD-BEARING as of 2026-09-20, and kept deliberately rather
    // than deleted. It was written because three missed marks demoted the node
    // and ended the very run being performed; MissedMarks is now retired
    // (TimedModePolicy.h), so the worst this suppresses is a diagnostic
    // counter. It stays because HW-2 is blocked mid-investigation and its
    // disabled witness still asserts consecutiveMissedMarks() == 0 — pulling it
    // now would disturb an open measurement to save nothing today. DELETE IT
    // when HW-2 closes, together with that witness.
    //
    // The offset is non-zero only for MODE_SWEEP (handleModeTest) and is cleared
    // on every exit path.
    bool sweepMisArming_() const
    {
        return this->mode_test_active_ && this->grid_.params.arm_offset_us != 0;
    }
    void noteMarkArmed(uint8_t generation)
    {
        if (this->mark_window_open_ && !this->sweepMisArming_())
        {
            // Armed, and neither hit nor closed empty: nothing addressed to this
            // node arrived at that mark. COUNTED, never demoted on — see the
            // Demotion banner in TimedModePolicy.h.
            if (this->missed_marks_ < 0xFFFF) this->missed_marks_++;
        }
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteWindowArmed();
        portEXIT_CRITICAL(&this->funnel_mux_);
        this->mark_window_open_ = true;
        this->mark_window_gen_  = generation;
    }
    // `generation` is the window the frame ARRIVED IN, carried with it from the
    // radio (rx_buffer_t::window_gen). A mark is kept only by a frame its own
    // window caught — the same rule noteMarkMissed already applies to closing.
    //
    // Before this gate existed the hit keyed on "addressed to me" alone, so any
    // frame heard by ANY window resolved whatever mark was open. Measured
    // consequence (review finding 13): a sweep deliberately mis-arming its
    // windows still reported `armed 189, hit 189` at every offset, including
    // +19 000 us (window opens 760 us AFTER T0) and -20 000 us (38 ms early).
    // WMR's hit half meant "the node was served", not "this window worked".
    //
    // kNoWindowGen: no generation travelled with the frame — every non-radio
    // path, including host-injected frames. Such a frame cannot show which
    // window heard it, so it resolves nothing.
    static constexpr uint8_t kNoWindowGen = 0xFF;
    void noteMarkHit(uint8_t generation)
    {
        if (!this->mark_window_open_) return;   // not a timed window
        if (generation == kNoWindowGen) return; // unknown window: proves nothing
        if (generation != this->mark_window_gen_) return;
        this->mark_window_open_ = false;
        portENTER_CRITICAL(&this->funnel_mux_);
        this->funnel_.noteWindowHit();
        portEXIT_CRITICAL(&this->funnel_mux_);
    }
    // The window closed with nothing usable in it. Advances the missed-mark
    // counter the demotion policy keys on — which counts "no frame ADDRESSED to
    // me at my mark", never "nothing received", because a window walked through
    // by another node's burst is not empty and would otherwise reset it.
    bool markWindowOpen() const { return this->mark_window_open_; }
    // `generation` is the window that closed. A window which is not the one the
    // mark was armed for closes nothing: it was never promised anything.
    void noteMarkMissed(uint8_t generation)
    {
        if (!this->mark_window_open_) return;
        if (generation != this->mark_window_gen_) return;
        this->mark_window_open_ = false;
        // The window still CLOSES during a sweep — the funnel's armed/hit counts
        // are exactly what the sweep measures — but see sweepMisArming_(): its
        // own induced misses must not demote it.
        if (this->sweepMisArming_()) return;
        if (this->missed_marks_ < 0xFFFF) this->missed_marks_++;
    }

    // M3 — MAC-1 / MAC-2 as switchable sublayers, for MAC CONTROL FRAMES ONLY
    // (mac-layer.md section 4). Application traffic is never affected.
    const macsublayers::Config &macSublayers() const { return this->sublayers_; }
    // B2 — phase tracking. The sample is COMMITTED only for frames addressed
    // to this node; see PhaseTracker.h on why stamping a neighbour's frame
    // makes phaseErrUs bimodal rather than merely noisy.
    const phase::Stats &phaseStats() const { return this->phase_; }
    void resetPhaseStats() { this->phase_.reset(); }
    phase::RtcSlowSrc rtcSlowSrc() const { return this->rtc_slow_src_; }
    void setRtcSlowSrc(phase::RtcSlowSrc s) { this->rtc_slow_src_ = s; }
    // Expected T0 for this node's next mark. Zero until a grid exists (B3),
    // and no sample is committed while it is zero — an expectation of "0"
    // would make every error look like the whole node uptime.
    void setExpectedT0Us(int64_t t0) { this->expected_t0_us_ = t0; }
    int64_t expectedT0Us() const { return this->expected_t0_us_; }

    // B3 — the published grid. Adopted only after an agreement check; see
    // GridState.h on why the anchor is solved locally rather than transferred.
    // --- B3: one receive window per round -------------------------------
    //
    // DEFAULT OFF. The design gates enabling this on T_detect being measured
    // (HW-2), because T_detect sets the entire late-side guard and is currently
    // a rule of thumb. Until that number exists, narrowing from three windows
    // to one trades a 3x reception margin for a battery saving on the strength
    // of an assumption. The mechanism ships; the switch waits for the number.
    void setTimedRxEnabled(bool v) { this->timed_rx_enabled_ = v; }
    bool timedRxEnabled() const    { return this->timed_rx_enabled_; }

    // Everything the mode needs, in one predicate. Uses TimedModePolicy so the
    // node and the hub reason about promotion with the same rules.
    bool timedRxActive() const;
    // Why the node is not in Mode B right now (a timedmode::Demotion value), or
    // 0 = None. The node decides; this is carried to the hub in PhaseReport.
    uint8_t demotionReasonNow() const;

    // Spend an uplink telling the hub our Mode B status changed, if one is due.
    //
    // timedRxActive() RAISES the flag (it is const, and the receive task calls
    // it on every arming pass); this SENDS, from task context, because
    // setStatus() is a queue push. Called from the arming path right after
    // timedRxActive(), which is the one place guaranteed to run whenever the
    // mode could have changed.
    //
    // Throttled by timedmode::shouldAnnounceModeChange. Without a floor a
    // marginal NoPhase <-> None oscillation costs one uplink per transition on
    // a battery node — reintroducing, as airtime, the flapping cost the
    // MissedMarks retirement removed.
    void announceModeChangeIfPending();

    // --- Section 4.3: placing the UPLINK ---------------------------------
    //
    // The lead between beginning CAD and the frame's T0: the CAD itself, the
    // PA ramp, the preamble, and the software in between.
    //
    // The two unmeasured terms are passed as ZERO on purpose. d_tx_ramp is
    // unmeasured (implementation-plan.md section 12.1) and the hub already
    // passes 0 for it in every placement it makes; d_cad_dispatch — the task
    // hop, the radio mutex and the register writes ahead of lora_cad() — has
    // never been measured either. A zero for each makes the uplink LATE by
    // exactly their sum, which is a knowable error inside a 14 080 us guard; a
    // number guessed here would be an unknowable one, and would look measured.
    static constexpr uint32_t kUplinkCadDispatchUs = 0;   // UNMEASURED
    static constexpr uint32_t kUplinkTxRampUs      = 0;   // UNMEASURED
    static constexpr int64_t  kUplinkAimLeadUs =
        (int64_t) loratiming::kUplinkPreambleToT0Us + (int64_t) loratiming::kCadUs
        + (int64_t) kUplinkCadDispatchUs + (int64_t) kUplinkTxRampUs;

    // When the TX path should BEGIN its CAD so this uplink lands on this node's
    // mark + ulOffsetUs. 0 means "not aimed" — the caller sends the old way,
    // with the random backoff.
    //
    // `max_wait_us` is the caller's, because the only honest bound on how long
    // a frame may wait for its mark is the delay that waiting REPLACES, and
    // that number lives in the transmit path.
    //
    // Not const: it maintains the hit/miss counters, which are the only
    // field-visible evidence that the offset is honoured.
    int64_t uplinkCadStartUs(int64_t now_us, int64_t max_wait_us);
    // U-4: the hub's observed in-slot count, as this node last heard it.
    uint32_t hubInSlotUplinks() const { return this->hub_in_slot_uplinks_; }
    uint32_t uplinkAimHits() const   { return this->ul_aim_hits_; }
    uint32_t uplinkAimMisses() const { return this->ul_aim_misses_; }

    // --- Section 4.4: the fleet key and the beacon's MAC -----------------
    //
    // Whether this node holds a fleet key at all. It is the switch between two
    // beacon policies, and the ratchet is deliberate:
    //
    //   no key  -> a beacon may still correct the anchor, bounded by the guard,
    //              and may never touch the pending mask. Exactly what shipped
    //              before, so a node that never receives a key does not regress.
    //   a key   -> a beacon must carry a matching id and a valid MAC or it is
    //              ignored ENTIRELY. Once forgery can be detected, tolerating an
    //              unsigned beacon would leave the attack open: the guard bounds
    //              one nudge, not a sequence of them, so an attacker beaconing
    //              freely walks the anchor 14 ms at a time until the node is off
    //              the grid.
    bool hasNetKey() const {
        return framecrypto::netKeyIsSet(this->net_key_, sizeof(this->net_key_),
                                        this->net_key_id_);
    }
    uint32_t netKeyId() const { return this->net_key_id_; }
    // Verify a beacon's MAC against the held key. False when there is no key,
    // when the id does not match, or when the tag is wrong — the caller does
    // not need to tell those apart, because all three mean "not from our hub".
    bool beaconMacIsValid(uint32_t net_key_id, uint32_t tx_round,
                          uint32_t tx_slot, uint32_t pending_mask,
                          bool pending_mask_valid,
                          const uint8_t *mac, size_t mac_len) const;

    // When to arm for the next mark. Meaningless unless timedRxActive().
    int64_t nextArmInstantUs(int64_t now_us) const;
    // How long to wait before arming, given the caller's own arm lead. The
    // clamping rules live in GridState.h with their tests.
    // Delay until the next window worth opening, and which kind it is. See the
    // definition: the beacon window is never skipped, and it is not a mark.
    // `opened_t0_us` is the T0 of the window already listening, which is never
    // armed again; `t0_out` receives the T0 this arm is aimed at.
    int64_t nextArmDelayUs(int64_t now_us, int64_t lead_us,
                           gridstate::WindowKind &kind_out,
                           int64_t opened_t0_us = 0,
                           int64_t *t0_out = nullptr) const;
    // Section 4.4's pending-data bitmap: false means the last beacon said the
    // hub has nothing for this node and that statement has not expired, so the
    // next window can be skipped. Always true when anything is uncertain.
    bool    shouldArmNextWindow(int64_t now_us) const;

    // B4, for tests: the cache sendCommandAck() populates. Exposed read-only
    // because the re-ack path's positive case needs an ENCRYPTED duplicate to
    // exercise end to end, and the host fixture has no encryption helper — so
    // the two halves are checked separately instead of not at all.
    const ackcache::Cache &ackCacheForTest() const { return this->ack_cache_; }
    // B3, for tests: whether adopting a grid switched timed RX on.
    bool timedRxEnabledForTest() const { return this->timed_rx_enabled_; }
    // Section 4.4, for tests: the pending bitmap as this node currently holds
    // it. shouldArmNextWindow() answers the question the radio asks, which is
    // not the same question — a node whose OWN bit is set arms either way, so
    // it cannot tell "adopted the mask" from "refused it".
    const pending::Mask &pendingStateForTest() const { return this->pending_; }
    // For tests: the stored base nonce for a peer, i.e. whether a session
    // exists. Read-only; set_base_nonce stays protected.
    bool getBaseNonceForTest(uint32_t peer, uint32_t &out) {
        return this->get_base_nonce(peer, out);
    }
    // For tests: seed an established session without replaying a LoginMsg.
    // handleLogin() carries an F-30 rate limit keyed on the REAL monotonic
    // clock (5 s), and the limiter's state is a function-static, so a test
    // that needs "a session already exists" cannot get there by sending two
    // logins — the second is dropped before it reaches any of the code under
    // test. Seeding the nonce directly is the only way to test the plaintext
    // gate's exemption without a five-second sleep.
    // Seed "this node already has a session", which is what the plaintext gate
    // now tests. It sets session_proven_ as well as the peer's nonce: the gate
    // deliberately asks about THIS NODE's state rather than about the sender
    // named on the wire, so seeding only the nonce would leave the node
    // sessionless and the gate correctly silent — a test that passed before by
    // sending from the one address the old, bypassable predicate happened to
    // work for.
    void setBaseNonceForTest(uint32_t peer, uint32_t nonce) {
        this->set_base_nonce(peer, nonce);
        this->session_proven_ = true;
    }
    // For tests: the downlink replay counter. What an injected frame is trying
    // to ratchet; the only way to see that it did not.
    uint32_t rxMsgIdForTest() const { return this->session_.rxId(); }

    // --- C2: Class A windows, placed from the node's own uplink -----------
    //
    // The mode needs no clock agreement with the hub: the windows hang off
    // T0_uplink, which the node derives from its own TxDone. See the banner in
    // the .cpp; the sequencing is ClassAWindows.h's.
    void noteUplinkSent(int64_t t_txdone_us, uint32_t uplink_len);
    void noteClassASleepOk(bool ok);
    // rx_us: the RxDone stamp when a frame arrived (node time), 0 otherwise.
    // Only used to log where in the window the frame landed.
    void noteClassAWindowResult(bool had_data, int64_t rx_us = 0);
    classa::WakeAction classAAction() const;
    // 0 means "no window to open" — never "now".
    int64_t classAArmInstantUs() const;
    int64_t classAArmDelayUs(int64_t now_us, int64_t lead_us) const;
    // The pending window's arm instant and nominal close, from ONE snapshot so
    // the two can never describe different windows. Both 0 when nothing is
    // pending.
    void    classAPendingWindow(int64_t *arm_us, int64_t *close_us) const;
    bool    classAActive() const { return this->classaSnapshot_().active; }

    // --- ModeTest (test-plan.md section 10) -------------------------------
    // Section 4.4's broadcast beacon: re-anchor within the guard, sample the
    // phase, adopt the pending bitmap. Carries no geometry — see the banner.
    // The hub's startup withdrawal. Takes only the header, because the message
    // has no fields — see blinds.proto. Unauthenticated by design; the handler
    // carries the two limits that make that safe (broadcast only, and the fleet
    // key is not cleared).
    void handleGridDemote(const LoraHeader *outer_header);
    void handleGridBeacon(LoraClientOperationMessage *message_to_process,
                          const LoraHeader *outer_header, int64_t rx_us);
    void handleModeTest(LoraClientOperationMessage *message_to_process,
                        const LoraHeader *outer_header, int64_t rx_us);
    // §4.6's promotion evidence, filled into every uplink that carries it (the
    // wake beacon and every CommandAck). Const: it reports what has been
    // measured and changes nothing. Public because the report is the node's
    // answer to a question the hub asks, and because it is the one part of
    // this path the host suite can reach — processTxCommand, which assigns it
    // into the outgoing message, is a task loop the harness does not run.
    void fillPhaseReport(PhaseReport &pr) const;

    bool modeTestActive() const { return this->mode_test_active_; }
    // The label the report will carry (`rep.mode = mt_mode_`), exposed so a
    // test can pin that it describes what the node APPLIED rather than what the
    // hub asked for — the defect this field shipped with.
    uint8_t modeTestReportedMode() const { return this->mt_mode_; }
    uint32_t modeTestLastRefusal() const { return this->mt_last_refusal_; }
    int32_t  modeTestPpmForTest() const { return this->mt_drift_fit_.ppm(); }
    int32_t  modeTestResidualPpmForTest() const { return this->mt_residual_fit_.ppm(); }
    uint32_t modeTestResidualSamplesForTest() const { return this->mt_residual_fit_.n; }
    uint32_t modeTestPpmSamplesForTest() const { return this->mt_drift_fit_.n; }
    // What the report will say the test RAN UNDER (`rep.powerProfileProduction`).
    // Exposed for the same reason modeTestReportedMode is: the field this
    // derives from spent its life reading as the opposite of its documentation,
    // and a test is the only thing that keeps a wire default honest.
    bool modeTestProductionProfile() const {
        return this->mt_power_profile_production_;
    }
    // The sample sinks are public so the radio-facing code (LoraInterface, the
    // handler task) can feed them without a friend declaration. They are inert
    // unless a test is running.
    void noteModeTestArmResidual(int32_t us) {
      if (this->mode_test_active_) this->mt_arm_residual_.add(us);
    }
    void noteModeTestTurnaround(int32_t us) {
      if (this->mode_test_active_) this->mt_turnaround_.add(us);
    }
    void noteModeTestOneShotError(int32_t us) {
      if (this->mode_test_active_) this->mt_one_shot_err_.add(us);
    }

    // One mark has passed. `addressed` is "a frame FOR ME arrived at my mark" —
    // NOT "something was received". A window walked through by another node's
    // burst is not empty, and keying on silence would let ordinary Mode A
    // traffic from a neighbour demote the very node this protects.
    void noteMarkOutcome(bool addressed);
    uint32_t consecutiveMissedMarks() const { return this->missed_marks_; }
    // Ask the hub for this node's grid again when timedmode::syncRequestReason
    // says so. Called on every addressed frame; public so a test can drive it.
    void maybeRequestGridSync();
    uint32_t gridSyncRequests() const { return this->gridsync_requests_; }

    const gridstate::State &gridState() const { return this->grid_; }
    int32_t clockRatePpbForTest() const { return this->clock_rate_ppb_; }
    gridstate::Refusal lastGridRefusal() const { return this->grid_refusal_; }

    void setBenchNode(bool v) { this->bench_node_ = v; }
    bool isBenchNode() const  { return this->bench_node_; }
    bool isSessionProven() const { return this->session_proven_; }

    // Long enough for the hub's post-beacon TimeSync (deferred ~750 ms after it
    // processes the frame) plus a burst window and one retransmit, short enough
    // that a failed resume still recovers well inside a wake.
    static constexpr uint32_t kResumeFallbackMs = 12000;

    // The hub's largest gap between two frames of one conversation (its
    // retransmit interval), which the REGISTER this fallback sends has to be
    // answered within.
    static constexpr uint32_t kHubInterFrameGapMs = 5000;

    // THE ORDERING THAT MAKES A LOST REPLY SURVIVABLE, as a build error rather
    // than a comment. The quiet window holds the node awake; inside it the
    // beacon ladder re-asks twice and, failing that, this fallback re-registers.
    // A window shorter than the fallback plus one hub gap means the node sleeps
    // before the escalation can run, so a node that lost three replies in a row
    // sleeps with session_proven_ false and recovers only on some later wake.
    // Found while pricing D-2: kQuietWindowMinMs was 10 000 against a 12 000
    // fallback, so the floor the YAML is allowed to configure was already past
    // that line even though the shipped default (20 s) was not.
    static_assert(automode::kQuietWindowMinMs >= kResumeFallbackMs + kHubInterFrameGapMs,
                  "the quiet window must outlast the REGISTER fallback plus one "
                  "hub inter-frame gap, or the fallback can never fire");

    // ---- beacon retry ladder ----
    // The link is asymmetric: every hub->node message is bursted 17x across a
    // full round because the node's receiver is windowed, but a node uplink is
    // sent ONCE. The wake beacon is therefore the least protected frame in the
    // system, and it is the one the whole exchange hangs on — the hub answers
    // it with TimeSync and, if the version differs, the schedule.
    //
    // Losing it used to cost a full REGISTER -> ClientConfig -> login handshake
    // (~4 s of radio), because armResumeFallback() was the only recovery and it
    // jumps straight there. Re-sending the beacon costs ONE frame.
    //
    // The hub always answers a beacon (handle_beacon_ schedules TimeSync
    // unconditionally at +750 ms, then bursts it), so a decrypted downlink is a
    // reliable implicit ACK — no protocol change needed to detect the loss.
    //
    // The ladder deliberately fits INSIDE the existing 12 s fallback window:
    //   t=4 s  no ack -> re-beacon      t=8 s  no ack -> re-beacon
    //   t=12 s still nothing -> REGISTER, exactly as before.
    void armBeaconRetry();
    void cancelBeaconRetry();

    // 750 ms hub deferral + a ~1.5 s burst + margin.
    static constexpr uint32_t kBeaconAckMs      = 4000;
    // Two retries keep the ladder inside kResumeFallbackMs even with jitter.
    static constexpr uint8_t  kMaxBeaconRetries = 2;
    // Breaks lockstep between two nodes whose beacons just collided: without
    // it they would retry in step and collide again.
    static constexpr uint32_t kBeaconRetryJitterMs = 500;

    // The other half of the ordering: the cheap retries must finish before the
    // fallback escalates, or a lost reply costs a full re-registration when two
    // re-beacons would have done. Asserted here rather than above because these
    // constants are declared after kResumeFallbackMs.
    static_assert(kResumeFallbackMs > 2 * kBeaconAckMs + 2 * kBeaconRetryJitterMs,
                  "the beacon retry ladder must finish before the REGISTER "
                  "fallback, or the cheap retries are skipped");

    // ---- deferred auto-sleep ----
    // Entering automatic mode must NEVER sleep the node the instant the
    // condition is met.  Observed live: a node woke, sent its REGISTER and
    // slept 3 s later — long before the hub's LoginMsg (deferred ~4 s) could
    // arrive.  The hub then retried login 24 times per cycle against a sleeping
    // node, the handshake could never complete, and the node was unreachable in
    // a 600 s loop while looking, from the outside, like a radio problem.
    //
    // The same shape bites after the handshake: TimeSync arrives ~1.25 s before
    // ScheduleConfig, and sleeping on TimeSync alone would miss the schedule
    // push and every one of its retransmits.
    //
    // So the sleep is armed as a QUIET TIMER instead: every downlink that could
    // start auto mode refreshes it, and the node sleeps only once the hub has
    // stopped talking to it.  If the hub never answers at all, it still fires,
    // so a node cannot stay awake burning battery.
    void armAutoSleep();
    void cancelAutoSleep();

    // Floor for a configured post_event_window. The window also holds us awake
    // THROUGH the handshake, so anything shorter than the hub's largest
    // inter-frame gap (5 s retransmits) would let the node sleep in the middle
    // of a conversation — F8 all over again, but self-inflicted from YAML.
    static constexpr uint32_t kAutoSleepQuietMinMs = automode::kQuietWindowMinMs;

    // The effective quiet window: post_event_window if configured, else the
    // default; floored as above.
    uint32_t autoSleepQuietMs();

    // ---- P3: automatic (scheduled) mode ----
    // shouldRunAutoMode() is the single gate: auto mode requires a VALID CLOCK
    // (I8 — a node that cannot evaluate its schedule must never sleep against
    // it) AND a schedule with at least one entry that can fire.
    bool     shouldRunAutoMode();
    uint64_t computeNextEvent();     // UTC epoch, 0 = nothing scheduled
    uint64_t computeSleepSeconds();  // 0 = do not sleep
    bool     runDueScheduleEntry();  // execute what is due, incl. catch-up

    // ---- D4: temporary interactive override ----
    // A button press must give whoever is standing at the blind a responsive
    // device, so it suspends automatic mode — but only TEMPORARILY. The hub's
    // configured mode stays authoritative and untouched; this is a local,
    // self-expiring override held in RTC memory so it survives a wake.
    //
    // interactiveTimeout == 0 means "stay interactive until told otherwise" (a
    // documented, meaningful zero in blinds.proto), and is represented here as
    // an override with no expiry.
    void     enterInteractiveMode();
    // Drop the interactive override so automatic mode resumes immediately.
    // Needed by the CMD_MODE_AUTO sysop: a node told to go auto while it is
    // awake must not keep sitting out an interactive window it entered from a
    // button press.
    void     clearInteractiveMode();
    bool     isTemporarilyInteractive();
    // Seconds until auto mode resumes; 0 when not overridden, UINT32_MAX when
    // the override never expires.
    uint32_t interactiveRemaining();

    // ---- P1: node wall clock (seeded by the hub's TimeSync) ----
    // State lives in RTC_DATA_ATTR so it survives deep sleep; these are static
    // because the clock is a property of the node, not of a dispatcher instance.
    // isClockValid() is what the scheduler will gate on: a node that has never
    // been told the time must not sleep against a schedule it cannot evaluate.
    static bool     isClockValid();
    static int32_t  getUtcOffset();
    static uint64_t getDstNext();

    // ---- Mode-announce state that must SURVIVE DEEP SLEEP ----
    // Same storage class and the same reason as the clock above: a wake keeps
    // it, a power-on clears it. See the note beside mode_announce_pending_.
    static uint8_t  lastReasonAnnounced();

    // A COLD BOOT, explicitly. RTC_DATA_ATTR is a no-op on the host, so this is
    // what a test uses to get a clean slate; on device the same clearing is
    // what a power-on does for free.
    static void     resetModeAnnounceState();

    // A DEEP-SLEEP WAKE, explicitly: clears exactly what plain RAM loses and
    // keeps exactly what RTC_DATA_ATTR retains.
    //
    // This exists so the RTC/plain split is the thing under test rather than
    // something implied by which C++ object a test happens to construct. A
    // bench that models a wake by building a second dispatcher tests object
    // lifetime, not the node — and that is how a fix which does nothing on
    // hardware passed the suite twice (1.1.2, 1.1.3).
    void            simulateWakeForTest();
    // Format an epoch as LOCAL wall time using the hub-supplied UTC offset.
    static void     formatLocalTime(uint64_t epoch, char *out, size_t out_len);

    void setBlindOperation(const blinds_syscmd_base_t cmd);
    void setSystemCommand(const blinds_syscmd_base_t cmd);
    void setMotorCommand(const MotorCmd_t cmd);

    void startBatteryMonitoring();
    // F-39: startBatteryMonitoringWithUDP / startMotorCurrentMonitorWithUDP removed
    //       2026-05-25 — dead code, tasks never spawned in production.
    void startMotorCurrentMonitor();
    void startOTA();
    void startWifi();
    void stopWifi();
    void enterDeepsleep();
    bool checkQueuesIdle();

    void setAddress(uint8_t cfgAddress, uint8_t cfgSubnet);
    void processRxCommand(void *pvParameter);
    void processSysCommand(void *pvParameter);
    void processTxCommand(void *pvParameter);
    // The body of that task, as a function. Extracted so it can be RUN: the
    // task blocks on portMAX_DELAY, so the host suite could not reach the code
    // that builds every uplink this node sends. Behaviour is unchanged — the
    // task calls this once per command.
    void serviceTxCommand(const tx_command_t &txcmd);
    // One non-blocking iteration of that task. False when the queue was empty,
    // so a caller can drain to quiescence and still tell "nothing queued" from
    // "queued, and it produced no frame".
    //
    // Public because it is the only way to exercise the real uplink path off
    // the MCU, and an end-to-end test that mirrored this code instead would be
    // asserting one side's arithmetic against itself — the mistake §11b's seam
    // entry exists to record.
    bool runOneTxCommand();
    // rx_us: the DIO0 arrival instant from the ISR. Defaulted so existing
    // callers (and the host test harness) are unaffected; 0 means "unknown",
    // which the drift estimator skips rather than treating as time zero.
    void onReceiveNew(uint8_t *rxBuf, int packetSize, int64_t rx_us = 0);

    // Per-command handlers, one per cmd_case, split out of onReceiveNew.
    //
    // That function was a single 479-line body with a cyclomatic complexity of
    // 70 — every command's logic sharing one scope with the decrypt path, the
    // replay check and the address filter. A `break` in the middle of it could
    // mean "stop handling this command" or "leave the inner switch", and the
    // cleanup at the bottom had to be duplicated at each of eight early exits.
    //
    // Each handler runs only after admitFrame() has accepted the frame, so none
    // of them repeats the address / replay / encryption checks.
    // Decrypt a downlink frame; returns a message the caller owns, or nullptr.
    LoraClientOperationMessage *decryptDownlink(LoraClientOperationMessage *rcv_message,
                                                const LoraHeader *outer_header);
    // Address filter, replay window and the plaintext-command gate. The ORDER
    // of the three is load-bearing — see the comments on the definition.
    bool admitFrame(LoraClientOperationMessage *msg, const LoraHeader *outer_header,
                    bool was_encrypted);

    // ---- clock-drift measurement (see DriftEstimator.h) --------------------
    // Collects burst copies and fits node-vs-hub drift. Opportunistic: in
    // normal windowed RX the node rarely hears 3 copies of one burst, so this
    // mostly does nothing until DRIFT_TEST_MODE puts the radio in continuous RX.
    void noteBurstArrival_(const LoraHeader *h, int64_t rx_us);
    drift::Sample drift_samples_[drift::kMaxSamples]{};
    uint8_t       drift_n_{0};
    int64_t       drift_copy0_ref_{0};
    drift::Accumulator drift_acc_{};
    // Drift-test lifetime. Deadline is on the node clock, so it holds even if
    // the hub goes away mid-test.
    bool     drift_test_active_{false};
    // Long-baseline fit across the WHOLE test. Frames are indexed by msgid, so
    // a lost frame leaves a gap rather than shifting everything after it.
    drift::LongFit drift_fit_{};
    uint32_t drift_first_msgid_{0};
    uint32_t drift_last_msgid_{0};
    bool     drift_have_first_{false};
    uint32_t drift_period_ms_{0};
    int64_t  drift_t0_us_{0};    // first sample; all indices are relative to it
    int64_t  drift_last_rx_us_{0};   // previous ACCEPTED frame
    uint32_t drift_idx_{0};          // grid index of that frame
    uint32_t drift_offgrid_{0};      // frames rejected as not on the grid
    // Arrival instant of the frame currently being dispatched, so the
    // per-command handlers can use it without re-plumbing every signature.
    int64_t  drift_rx_us_{0};
    esp_timer_handle_t drift_test_timer_{nullptr};



    void dispatchCommand(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleNotSet(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleOperation(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleSysop(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleClientConfig(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleTimeSync(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleSchedule(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleDriftTest(LoraClientOperationMessage *msg, const LoraHeader *outer_header);

    // SystemCtrl consults this before entering deep sleep: a drift test must
    // not be truncated, because its whole value is the long baseline.
    // (This region is already public — do NOT add access specifiers here; an
    // earlier attempt flipped everything below to private and broke the
    // harness at link time, not at the edit site.)
    bool driftTestActive() const { return this->drift_test_active_; }

    // A timing sample: called for EVERY received frame while a drift test is
    // running, straight from the DIO0 task, before any parsing.
    //
    // The test does not care what the frame says. Decoding it would only add
    // failure modes -- plaintext frames are rejected once a session exists, a
    // replayed msgid is dropped, a CRC failure discards the packet -- and every
    // one of those would silently throw away a perfectly good timestamp. The
    // arrival instant is the entire measurement.
    void noteDriftSample(int64_t rx_us);
    // Ends the drift test on a one-shot timer, so a lost "off" command -- or a
    // hub that stops transmitting -- cannot strand the node in ~11 mA
    // continuous RX. See the comment on the definition.
    static void driftTestExpiredCb_(void *arg);
    void stopDriftTest_();
    void handleLogin(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleBaseNonce(LoraClientOperationMessage *msg, const LoraHeader *outer_header);
    void handleCoverConfig(LoraClientOperationMessage *msg, const LoraHeader *outer_header);

    void restorePosition(){
        motCtrl->restorePosition();
    }

    // F-29: restoreMessageIds / getMessageIds removed 2026-05-25.
    //       Message IDs are always reset to 0 on boot and re-negotiated via LoginMsg.

    void stopLoraPollingTimer() { loraIf->stopLoraPollingTimer(); }

    MotorCtrl *motCtrl{NULL};
    SystemCtrl *sysCtrl{NULL};
    LoraInterface *loraIf{NULL};

    QueueHandle_t rxCmdQueueNew;
    QueueHandle_t txCmdQueueNew;
    QueueHandle_t sysCmdQueueNew;

    Packet rxPacket;
    Packet txPacket;
    // F-4: destAddress is used both as the LoraHeader.destaddress of replies and
    // as the key for get_base_nonce().  set_base_nonce() stores under the full
    // 32-bit senderaddress, so these must be 32-bit too — a uint8_t here
    // truncated any peer address > 0xFF, the nonce lookup missed, and the reply
    // (e.g. the cover ACK) went out unencrypted and was dropped by the hub.
    uint32_t destAddress; // destination to send to
    uint32_t destSubnet;  // destination to send to
    uint8_t cfgAddress;  // address of this device
    uint8_t cfgSubnet;   // address of this device

protected:
    // Session state (peer nonces, msgid counters, replay window, NVS blob)
    // lives in SessionManager. It was extracted because three real defects hid
    // in it — a persist key that never tracked the hub, a zero nonce reported
    // as a session, and a truncated restore — all found on hardware because the
    // state was only reachable through a fully-constructed CmdDispatcher.
    SessionManager session_;

    // Estimated end-of-burst time (esp_timer_get_time() us); written by
    // onReceiveNew, read by LoraInterface::loraRxTask.  volatile + 8-byte value:
    // a torn 64-bit read on the 32-bit core would at worst mis-time the deferral
    // slightly, which CAD absorbs.
    volatile int64_t burst_end_us_{0};
    MacCounters mac_{};
    macfunnel::Counters funnel_{};
    portMUX_TYPE        funnel_mux_ = portMUX_INITIALIZER_UNLOCKED;
    macsublayers::Config sublayers_{};
    // Highest MAC-ping `seq` accepted while MAC-1 is on; see the counter check.
    uint32_t mac_ping_seq_hw_{0};
    esp_timer_handle_t   sublayer_restore_timer_{nullptr};
    // True only while dispatching a frame that arrived through the AEAD path.
    // Set per frame in onReceiveNew; a handler must never infer authentication
    // from anything else, because the outer header is plaintext and forgeable.
    bool                 frame_authenticated_{false};
    // Gates the deliberately-degrading configurations. FALSE in the field and
    // provisioned explicitly for a bench unit: a hardcoded true made
    // ArmRefusal::NotBenchNode unreachable, so every field node accepted a
    // degrading config exactly as a bench node would.
    // Bench units unlock the modes that deliberately break reception. This is
    // a BUILD-TIME flag (CONFIG_BLINDS_BENCH_NODE, see main/Kconfig) and
    // deliberately not settable over the air: the only plausible carrier,
    // ClientConfig, is accepted unauthenticated and gated only by a MAC match
    // that is broadcast in the clear. setBenchNode() remains for tests.
#ifdef CONFIG_BLINDS_BENCH_NODE
    bool                 bench_node_{true};
#else
    bool                 bench_node_{false};
#endif
    // A timed window is open, so the next receive outcome belongs to a mark.
    volatile bool        mark_window_open_{false};
    // Which receive window that mark was armed for (LoraInterface's window
    // generation). Only that window may close it.
    volatile uint8_t     mark_window_gen_{0};

    // --- ModeTest state ---------------------------------------------------
    bool                    mode_test_active_{false};
    esp_timer_handle_t      mode_test_timer_{nullptr};
    int64_t                 mt_started_us_{0};
    uint8_t                 mt_mode_{0};
    bool                    mt_power_profile_production_{true};
    bool                    mt_mac_echo_{false};
    uint32_t                mt_last_refusal_{0};
    modetest::SavedState    mt_saved_{};

    // The last pending-data bitmap a beacon carried. Zeroed = never had one =
    // listen; see PendingData.h.
    pending::Mask           pending_{};

    // B4: the last command we acked, so a byte-identical retransmit gets the
    // ack again rather than a silent drop. See AckCache.h — the whole design is
    // in telling a burst copy apart from a genuine retry, and the discriminator
    // is time.
    ackcache::Cache         ack_cache_{};

    // Mode C wake clock: the beacon just packed, waiting for its TxDone. Set by
    // the TX task, consumed by the DIO0 task on the next TxDone; the two are
    // sequential under the radio semaphore.
    volatile bool           beacon_tx_pending_{false};
    uint32_t                beacon_tx_pending_msgid_{0};

    // C2 state: where this node's last uplink put its windows, and how far
    // through the RX1 -> RX2 -> sleep sequence it is.
    struct ClassAState {
      bool              active{false};
      int64_t           t0_uplink_us{0};
      classa::WakeState state{};
    } classa_{};
    // classa_ is written from the DIO0 interrupt task (core 1) — noteUplinkSent
    // on TX_DONE, noteClassAWindowResult on RX_DONE/RX_TIMEOUT — and read from
    // the receive task (core 0) when it decides what to arm. t0_uplink_us is 64
    // bits, so on a 32-bit core a read can tear across a concurrent write: the
    // reader arms a one-shot for a garbage delay AFTER the periodic timer has
    // been stopped, and the node goes deaf until it expires. Same defect class
    // the funnel's counters were locked against; this is the protocol state the
    // fix did not reach.
    mutable portMUX_TYPE classa_mux_ = portMUX_INITIALIZER_UNLOCKED;

    // One consistent copy, taken under the lock. Every reader works from a
    // snapshot rather than touching classa_ field by field, so `active` and
    // `t0_uplink_us` can never come from different instants.
    ClassAState classaSnapshot_() const {
        portENTER_CRITICAL(&this->classa_mux_);
        ClassAState s = this->classa_;
        portEXIT_CRITICAL(&this->classa_mux_);
        return s;
    }
    modetest::SeqTracker    mt_seq_{};
    // The funnel as it stood when the test armed. The report is the DELTA
    // against it — see sendModeTestReport_.
    macfunnel::Counters     mt_funnel_base_{};
    modetest::Samples       mt_phase_err_{};
    modetest::Samples       mt_arm_residual_{};
    modetest::Samples       mt_turnaround_{};
    modetest::Samples       mt_one_shot_err_{};
    // Run-scoped clock-rate fit: each ModeTest mark's T0 against its nearest
    // mark index times the grid period. The only thing in Mode B that measures
    // the goal of Mode B — drift_fit_ is fed by DriftTest alone. See
    // noteModeTestMarkForRate_.
    drift::LongFit          mt_drift_fit_{};
    bool                    mt_have_first_t0_{false};
    int64_t                 mt_first_t0_us_{0};
    int64_t                 mt_last_k_{0};
    uint32_t                mt_grid_period_ms_{0};
    // Residual rate: the drift of the node CORRECTED prediction over the run.
    // Fed as (predicted span, measured span) from the first mark, so the fit
    // slope minus one IS the residual rate. Mode B pass line judged on it.
    drift::LongFit          mt_residual_fit_{};
    bool                    mt_have_first_pred_{false};
    int64_t                 mt_first_pred_us_{0};

    void        stopModeTest_();
    void        sendModeTestReport_();
    void        resetModeTestAccumulators_();
    void        noteModeTestMarkForRate_(int64_t rx_us, const LoraHeader *outer_header);
    static void modeTestExpiredCb_(void *arg);
    static const char *modeTestRefusalName_(modetest::ArmRefusal r);
    phase::Stats         phase_{};
    phase::RtcSlowSrc    rtc_slow_src_{phase::RtcSlowSrc::Unknown};
    int64_t              expected_t0_us_{0};
    // On-air payload length of the frame being dispatched, needed to recover
    // T0 from RxDone. Set once per frame in onReceiveNew, on the same task
    // that reads it — unlike drift_rx_us_, which two tasks write.
    int                  last_rx_len_{0};
    // The receive window the frame being dispatched actually arrived in, taken
    // from rx_buffer_t::window_gen at the unpack site. Same discipline as
    // last_rx_len_ above: written once per frame on the task that reads it.
    //
    // This is what lets a mark be booked HIT only by the window that armed it
    // (review finding 13). Sentinel 0xFF = "no generation travelled with this
    // frame", which is how every non-radio path (host tests, injected frames)
    // behaves, and it is treated as "do not resolve a mark".
    uint8_t              last_rx_window_gen_{0xFF};
    gridstate::State     grid_{};
    // MAC-0 clock discipline. The node clock rate against the hub, ppb, LEARNED
    // from beacons. Kept here and not only in grid_, because grid_.clear() runs on
    // every demote, withdrawal and re-adoption, and the rate describes this node
    // clock, not the grid: losing it would restart the drift from zero each time.
    int32_t              clock_rate_ppb_{0};
    // The last accepted beacon round: the start of the span the next beacon
    // residual is divided by. Cleared on adoption, when the anchor is new.
    bool                 have_rate_ref_{false};
    // Where that span starts, as hub-grid microseconds (gridstate::hubInstantUs):
    // a beacon, a stamped GridSync on a settled clock, or the provisional re-solve.
    int64_t              rate_ref_hub_us_{0};
    gridstate::Refusal   grid_refusal_{gridstate::Refusal::None};
    bool                 timed_rx_enabled_{false};
    // Section 4.4's fleet key: what the broadcast beacon is signed under.
    // Adopted only from an AUTHENTICATED GridSync — a plaintext one may not
    // install a key, or the construction is decorative. Volatile on purpose:
    // it is re-published whenever the grid is, and a key that outlived the grid
    // would authenticate beacons for an anchor that no longer exists.
    uint8_t              net_key_[framecrypto::kNetKeyBytes]{};
    uint32_t             net_key_id_{0};
    // Section 4.3. How often the uplink actually made its mark, and how often
    // it could not — the field's own answer to HW-7.
    uint32_t             ul_aim_hits_{0};
    uint32_t             ul_aim_misses_{0};
    uint32_t             missed_marks_{0};
    // The msgId of the frame the last phase sample came from, so copies of one
    // burst count as one frame (phase::Stats::frames).
    uint32_t             last_phase_msgid_{0};
    // The demotion reason timedRxActive() last logged, so it logs a change once
    // rather than on every pass of the receive task. 0xFF = nothing logged yet.
    mutable uint8_t      last_reason_logged_{0xFF};
    // The grid was adopted before node time settled (nodeclock::settled()), so its
    // anchor is not trusted: no phase samples against it, and the first stamped
    // frame after settling re-solves it.
    bool                 grid_provisional_{false};
    // U-4. How many of this node's uplinks the HUB has seen land in its slot,
    // consecutively, as of the last TimeSync. §4.6's own promotion criterion,
    // and it cannot be measured here: the placement comes out of this node's
    // TRANSMIT path and the question is where the frame ARRIVED, so only the
    // hub can answer it — "a beacon saying I am ready says nothing about where
    // its window actually landed".
    //
    // Starts at 0, which means "not confirmed" and keeps the node in Mode A.
    // That is the right way round: proto3 omits defaults, so a hub predating
    // the field reads as unconfirmed and costs airtime rather than a command.
    // The node can still earn the count from Mode A, because the uplink aim
    // places its frames on its mark whatever mode it is in.
    uint32_t             hub_in_slot_uplinks_{0};
    // When a frame addressed to this node last arrived. 0 = never.
    int64_t              last_addressed_us_{0};
    // When the grid's anchor was last set or corrected (GridSync, beacon, settled
    // re-solve, trial re-centre), phase samples refused beyond half a pitch since
    // then, and when this node last asked the hub for its grid. 0 = never.
    int64_t              last_anchor_fix_us_{0};
    uint32_t             refused_since_fix_{0};
    int64_t              last_sync_request_us_{0};
    uint32_t             gridsync_requests_{0};
    void                 noteAnchorFixed_();
    // Telling the hub our Mode B status changed (timedmode::kModeAnnounceMinS).
    //
    // The hub learns this ONLY from a PhaseReport, which rides an uplink. An
    // interactive node never sleeps, never wakes, never beacons, and with no
    // traffic has nothing to ack — so without this the hub's belief about a
    // promoted, quiet node is stale indefinitely and it can never grant single
    // shot. Measured 2026-09-20: 12.6 min in Mode B, hub reporting reason 5.
    //
    // Two fields, not one. last_reason_logged_ is what the CONSOLE last saw;
    // this is what the HUB was last told. They are different audiences and
    // conflating them would either spam the log or skip an announcement,
    // depending on which won.
    //
    // mutable: the edge is detected inside timedRxActive(), which is const and
    // already mutates last_reason_logged_ the same way. The uplink itself is
    // NOT sent from there — announceModeChangeIfPending() does that from the
    // arming path, because setStatus() is a queue push and belongs in task
    // context, not behind a const accessor.
    // WHAT THE HUB WAS LAST TOLD, and WHEN, now live in RTC_DATA_ATTR statics
    // (s_last_reason_announced / s_last_mode_announce_us in the .cpp) rather
    // than here. As plain members every deep-sleep wake reset them, so
    // s_since read 0xFFFFFFFF (the throttle could never suppress) and any
    // current reason compared unequal to 0xFF (every wake was an edge). The
    // node therefore announced on EVERY wake and then sent its check-in
    // beacon too: two uplinks where one carries the same information.
    // Measured fw 1.1.2 AND 1.1.3, 2026-09-21: 2 MODE_CHANGED + 2
    // TIMER_CHECKIN across two Mode C wakes.
    //
    // Exactly the defect s_lastBatteryVoltage had — a plain member the wake
    // reset, so every beacon reported v=0.00 for a healthy battery.
    //
    // The PENDING FLAG deliberately stays a plain member: it is an intent,
    // not a fact about the hub. A wake must recompute it from the edge rather
    // than resurrect an announcement the node already decided about.
    mutable bool         mode_announce_pending_{false};
    // F-30 login rate limit: when the last accepted LoginMsg arrived, in ms
    // on the monotonic clock. 0 = none yet. A member rather than a
    // function-static so it dies with the dispatcher (see handleLogin).
    uint64_t             last_login_ms_{0};
    // Per-copy spacing of a hub burst. Taken from the shared LoraTiming.h
    // rather than restated, because "MUST match the hub" in a comment is not a
    // mechanism: this was a bare 88 whose only link to the hub was prose.
    static constexpr int kBurstTxIntervalMs =
        (int) (loratiming::kBurstCopyStrideUs / 1000);
    static_assert(kBurstTxIntervalMs == 88, "hub burst stride changed");

    // PersistState / kPersistMagic / kPersistTxMargin moved to
    // SessionManager.cpp. The on-NVS format is unchanged, so an existing blob
    // still loads.
    // P2b: set by noteSessionProven() when a downlink DECRYPTS successfully —
    // proof the hub still holds the same base nonce we do.  volatile: written
    // from the RX path, read from the esp_timer task.
    volatile bool session_proven_{false};
    esp_timer_handle_t resume_timer_{nullptr};
    static void resumeFallbackCb(void *arg);

    esp_timer_handle_t auto_sleep_timer_{nullptr};
    static void autoSleepCb(void *arg);

    esp_timer_handle_t clock_retry_timer_{nullptr};
    static void clockRetryCb(void *arg);

    esp_timer_handle_t beacon_retry_timer_{nullptr};
    static void beaconRetryCb(void *arg);
    uint8_t beacon_retries_{0};
    // D4: fires when the interactive override expires, putting the node back to
    // sleep against its schedule.
    WakeReason wake_reason_{WAKE_REASON__WAKE_BOOT};
    esp_timer_handle_t interactive_timer_{nullptr};
    static void interactiveExpiredCb(void *arg);
    esp_timer_handle_t register_retry_timer_{nullptr};
    static void registerRetryCb(void *arg);
    void startRegisterRetry_();   // arm unconditionally (sendRegister gives up the session)
    bool hasHubSession_();        // a base nonce for the hub we persist for
    void maybePersist_();

    // F-22: Pre-imported PSA key handle — key is imported once at startup.
    psa_key_id_t aes_gcm_key_id_{PSA_KEY_ID_NULL};

    // Encryption helpers (GCM)
    bool init_psa_key();
    bool encrypt_payload_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                             const uint8_t *plain, size_t plain_len, uint8_t *cipher, uint8_t *tag, size_t tag_len = 16);
    bool decrypt_payload_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                             const uint8_t *cipher, size_t cipher_len, const uint8_t *tag, size_t tag_len,
                             uint8_t *plain_out);

    bool derive_gcm_nonce(uint32_t peer_address, uint64_t frame_counter, uint8_t nonce_out[12]);
    bool get_base_nonce(uint32_t peer_address, uint32_t &base_nonce_out);
    void set_base_nonce(uint32_t peer_address, uint32_t base_nonce);
    // Invalidate a peer's stored base nonce so subsequent messages fall back to
    // plaintext until a fresh nonce is negotiated (via CMD_LOGIN).  Used when
    // (re-)registering: the restored NVS nonce is stale once we restart the
    // register->login handshake.
    void clear_base_nonce(uint32_t peer_address);

    bool send_tx_buffer(const uint8_t *buf, size_t len);
    bool pack_response_message(const LoraClientResponseMessage *message, uint8_t **out_buf, size_t *out_len);

    // F-25: Replace std::map base-nonce map with a fixed-size array to avoid
    //       heap fragmentation from std::map on embedded targets.
    // The AES-GCM frame counter is now the same value as LoraHeader.msgid
    // (unified counter), so per-peer tx_count / rx_count are not needed.

    // F-25: Peer counter helpers (tx/rx count helpers removed — unified counter)

    // base_nonce_map_ replaced by PeerCounter::base_nonce above.

    portMUX_TYPE &motorMux;
    portMUX_TYPE &buttonMux;

    // Initialize memory pool and queues
    esp_err_t init_memory_pool(void);

    // Last-known-good battery voltage — written by taskBatteryMonitor via
    // setBatteryVoltage(), read by processTxCommand() with no blocking wait.
    // Last-known-good battery voltage; initialised to 0.0 V, updated by
    // setBatteryVoltage() after each taskBatteryMonitor measurement.
    // lastBatteryVoltage_ moved to a file-scope RTC_DATA_ATTR in
    // CmdDispatcher.cpp — see the comment there.

    // F-41: true while a battery measurement (recovery wait + sampling) is in
    // progress; checked by checkQueuesIdle() before entering deep sleep.  The
    // deep-sleep drain-wait has a 120 s hard cap, so a stuck flag cannot keep
    // the node awake indefinitely.
    volatile bool batteryMeasurementBusy_{false};

    // F-23: rx_memory_pool and its queues made private; external access
    //       through get_free_buffer() / submit_rx_buffer() accessors below.
    rx_buffer_t rx_memory_pool[POOL_SIZE];
    QueueHandle_t rx_free_buffer_queue;
    QueueHandle_t rx_data_queue;
    SemaphoreHandle_t rx_pool_mutex;

public:
    SemaphoreHandle_t txbuf_mutex;

    // F-23: Accessor methods replace direct queue access from LoraInterface.
    // Get a free buffer from the pool
    rx_buffer_t *get_free_buffer(TickType_t timeout);

    // Submit a filled rx_buffer to the data queue for processing.
    // Returns ESP_OK on success, ESP_FAIL if queue is full (caller must
    // return the buffer to the pool via return_buffer_to_pool in that case).
    esp_err_t submit_rx_buffer(rx_buffer_t *buf);

    // Return a buffer to the free pool
    esp_err_t return_buffer_to_pool(rx_buffer_t *buffer);

    // Data processing task - Consumer
    void loraCommandProcTask(void *pvParameters);

    // F-39: stats_task declaration removed 2026-05-25 — dead code, never spawned.

    void new_deep_sleep_task(void *pvParameters);
};
