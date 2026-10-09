// Phase 3 — exercise the REAL production lora_client.cpp via host shims.
//
// The class under test is esphome::lora_tracker::LORAListener defined in
// local_components/lora_client/lora_client.h. Its scheduler and NVS calls
// route into the same SimClock + nvs_slot store the model-based tests use,
// so the assertions and timing logic transfer directly.
//
// This is the regression test for the IV-mismatch bug actually running
// AGAINST the production code, not a parallel model.

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/homeassistant/time/homeassistant_time.h"
#include "esphome/components/loracover/cover/lora_cover.h"

// The MAC tests build frames with the REAL generated stubs, so the bytes
// are exactly what goes on the air.
#include "blinds.pb-c.h"
#include "TimedGrid.h"
#include "ClassAWindows.h"
#include "LoraTiming.h"

#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/wire_codec.h"
#include "sim/crypto.h"

#include <psa/crypto.h>

#include <gtest/gtest.h>
#include "esphome/components/switch/switch.h"

using esphome::lora_tracker::LORAClient;
using esphome::lora_tracker::LORATracker;
using esphome::time::RealTimeClock;

namespace {

constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

// Tier 3 (mac-separation-implementation-plan.md section 2(b)): the
// deterministic node_nonce every session-establishing helper in this file
// uses (attach_encrypted_login_ack, and anything built on drive_session()).
// A real node mints this with esp_random(); a fixed, known value is what a
// test needs to re-derive the same K_enc/K_mac later (e.g. decrypt_downlink
// verifying what the hub sent).
constexpr uint32_t kTestNodeNonce = 0xACE55001u;

// Production initialises PSA Crypto in LORAListener::setup(). These scenarios
// construct the listener directly and never call setup() (it would also restore
// NVS and schedule its own logins, which each scenario controls itself), so PSA
// must be initialised here or every key import silently fails — and that is the
// ONE branch in decrypt_payload_gcm that returns false without logging.
void ensure_psa_ready() { ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS); }

// Answer every outgoing LoginMsg with a properly ENCRYPTED ClientAvailable,
// exactly as the node's CMD_LOGIN handler does (store the nonce, then
// sendAvailable()).  The hub sets login_acked_ ONLY inside the successful-
// decrypt branch — a successful decrypt is what proves the node holds the
// matching base nonce — so a plaintext reply does NOT acknowledge a challenge.
// See lora_client.cpp: "Only now do we treat login as acknowledged".
void attach_encrypted_login_ack(proto_sim::SimRadio& radio,
                                LORAClient& listener,
                                uint32_t node_addr,
                                uint32_t subnet) {
    radio.add_sink([&radio, &listener, node_addr, subnet](const proto_sim::AirFrame& f) {
        if (f.dir != proto_sim::AirFrame::Dir::HubToNode) return;
        auto m = proto_sim::as_op(f);
        if (!m || m->cmd != proto_sim::LoraClientOperationMessage::Cmd::Login) return;
        // Only the node the login is ADDRESSED to answers it. With two nodes on
        // one radio an unfiltered ack answers every login as every node.
        if (m->header.destAddress != node_addr) return;

        const uint32_t base_nonce = m->login.nonce;
        constexpr uint32_t kMsgId = 1;   // node's first post-login-reset tx
        // Tier 3: this IS the session-opening uplink for this test's
        // purposes (the real node's first post-login uplink is the wake
        // beacon, which this simplified ack stands in for) — it must carry
        // sessionNonce, or the hub never derives K_enc/K_mac and nothing
        // past this point can decrypt.
        proto_sim::LoraClientResponseMessage inner;
        inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
        inner.header.destSubnet    = subnet;
        inner.header.senderAddress = node_addr;
        inner.header.msgId         = kMsgId;
        inner.header.sessionNonce  = kTestNodeNonce;
        inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
        inner.avail.available      = true;

        // Payload-only plaintext (the inner header is stripped; the receiver
        // uses the outer one). Encrypt-then-CMAC seals once (CTR) and tags
        // over the outer header's fields (burst/fire fields all 0/false —
        // uplinks never burst) plus the ciphertext.
        auto plain = proto_sim::serialize_resp_payload(inner);
        framecrypto::EtmHeaderFields fields{};
        fields.downlink       = false;
        fields.session_id     = base_nonce;
        fields.dest_address   = inner.header.destAddress;
        fields.dest_subnet    = inner.header.destSubnet;
        fields.sender_address = inner.header.senderAddress;
        fields.msgid          = inner.header.msgId;
        auto enc = proto_sim::encrypt_then_cmac_seal(base_nonce, kTestNodeNonce,
                                                     esphome::lora_tracker::kHubAddress, node_addr,
                                                     /*downlink=*/false, fields,
                                                     plain.data(), plain.size());

        proto_sim::LoraClientResponseMessage outer;
        outer.header               = inner.header;
        outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
        outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
        outer.encrypted.ciphertext = enc.ciphertext;

        auto ack_bytes = proto_sim::serialize_resp(outer);
        listener.set_response(ack_bytes.data(), ack_bytes.size());
    });
}

// Phase 3 regression of scenario B2: the production send_login() must reuse
// the same pending base nonce on every retry within a challenge cycle.
// This is the bug shipped in this session's fix; rebuilding with the fix
// reverted causes this test to fail (verified manually).
TEST(RealLoraClient, SendLoginReusesPendingNonceAcrossRetries) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;

    // Wire shim hooks so the production set_timeout/set_interval/send/NVS
    // routes through our test harness.
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol_2;
    rol_2.set_name("rol_2");
    rol_2.set_short_address(18);
    rol_2.set_subnet_address(2);
    rol_2.set_sleep_duration(21600);
    rol_2.set_address(kMacRol2);

    RealTimeClock time;
    time.set_now(0, /*valid=*/false);  // suppress startup-login NTP path
    rol_2.set_time(&time);

    tracker.register_client(&rol_2);  // sets parent_; without this,
                                      // send_login()'s guard would bail

    // Force registered_=true (production normally sets this from REGISTER /
    // NVS restore). We don't want the REGISTER path's own login scheduling
    // here — we want to call send_login() directly and observe the nonce
    // reuse invariant in isolation.
    rol_2.registered_ = true;

    // First challenge.
    rol_2.send_login();
    const uint32_t pending_first = rol_2.pending_login_nonce_;
    ASSERT_NE(pending_first, 0u);

    // Two more retries before any ACK.
    rol_2.send_login();
    rol_2.send_login();

    EXPECT_EQ(rol_2.pending_login_nonce_, pending_first)
        << "REAL LORAListener::send_login() must reuse pending nonce on "
           "every retry — this is the production-side guarantee that "
           "the harness model also enforces.";

    // Every LoginMsg on the wire carries the same nonce.
    uint32_t observed = 0;
    int count = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (!m || m->cmd != proto_sim::LoraClientOperationMessage::Cmd::Login) continue;
        if (count == 0) observed = m->login.nonce;
        else EXPECT_EQ(m->login.nonce, observed)
            << "Production wire LoginMsg #" << (count + 1)
            << " has a different nonce than the first — pending-nonce "
               "fix must be in place.";
        ++count;
    }
    EXPECT_EQ(count, 3) << "Expected three LoginMsgs on the wire";
    EXPECT_EQ(observed, pending_first);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Regression test for the do_login_and_arm_retry_ ordering fragility:
// the reset of login_acked_/login_retry_count_ MUST happen BEFORE
// send_login(). With async radio this is harmless ordering; with sync
// radio (the test harness, or a hypothetical future driver swap) the
// post-call reset would clobber an already-arrived ACK and leave the hub
// thinking login was never acknowledged.
//
// We drive the real do_login_and_arm_retry_ via the REGISTER → 500 ms
// timer path and inject a synchronous ACK into the LoginMsg send. End
// state with the fix: login_acked_ stays true. End state without the
// fix: login_acked_ is clobbered to false by the post-call reset.
TEST(RealLoraClient, DoLoginAndArmRetryResetsBeforeSend) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;

    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol_2;
    rol_2.set_name("rol_2");
    rol_2.set_short_address(18);
    rol_2.set_subnet_address(2);
    rol_2.set_sleep_duration(21600);
    rol_2.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol_2.set_time(&time);
    tracker.register_client(&rol_2);

    // Every outgoing LoginMsg gets a synchronous, correctly encrypted ACK fed
    // back through the real LORAListener::set_response.
    attach_encrypted_login_ack(radio, rol_2, /*node_addr=*/18, /*subnet=*/2);

    // Drive the production REGISTER path, which schedules the login challenge
    // kRegisterToLoginDelayMs after the REGISTER (4 s today — it was raised
    // from 500 ms so the LoginMsg is not queued on top of the config bursts).
    proto_sim::LoraClientResponseMessage reg_msg;
    reg_msg.proto        = proto_sim::LoraClientResponseMessage::Proto::Register;
    reg_msg.reg.mac_addr = kMacRol2;
    auto reg_bytes = proto_sim::serialize_resp(reg_msg);
    rol_2.set_response(reg_bytes.data(), reg_bytes.size());

    // Fire that timer → do_login_and_arm_retry_ → send_login →
    // (sync sink inside the radio dispatch) → LORAListener::set_response
    // processes the Available reply → login_acked_ flips to true.
    // Derived from the production constant so a future retune does not
    // silently turn this regression test into a no-op.
    clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 100);

    EXPECT_TRUE(rol_2.login_acked_)
        << "do_login_and_arm_retry_ must reset login_acked_=false BEFORE "
           "calling send_login(). Otherwise a synchronous ACK arriving "
           "during send_login() is silently clobbered, and the hub "
           "spends the next 24 hours retrying an already-acknowledged "
           "challenge.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// =============================================================================
// Additional real-code scenarios — port the model-side coverage onto the
// production LORAListener so the test transcript actually exercises
// production source lines.
// =============================================================================

namespace real_helpers {

inline std::vector<uint8_t> serialize_register(uint64_t mac,
                                              bool needs_config = false) {
    proto_sim::LoraClientResponseMessage m;
    m.proto            = proto_sim::LoraClientResponseMessage::Proto::Register;
    m.reg.mac_addr     = mac;
    m.reg.needs_config = needs_config;
    return proto_sim::serialize_resp(m);
}

inline std::vector<uint8_t> serialize_avail(uint8_t sender, uint32_t msg_id) {
    proto_sim::LoraClientResponseMessage m;
    m.header.senderAddress = sender;
    m.header.msgId         = msg_id;
    m.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
    m.avail.available      = true;
    return proto_sim::serialize_resp(m);
}

struct RealHubHarness {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker  tracker;
    LORAClient   rol;
    RealTimeClock time;

    explicit RealHubHarness(uint8_t addr, uint64_t mac) {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

        rol.set_name("rol");
        rol.set_short_address(addr);
        rol.set_subnet_address(2);
        rol.set_sleep_duration(21600);
        rol.set_address(mac);
        time.set_now(0, /*valid=*/true);  // NTP-synced from boot
        rol.set_time(&time);
        tracker.register_client(&rol);
    }
    ~RealHubHarness() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

} // namespace real_helpers

// ---------------------------------------------------------------------------
// A GridSync is re-published until the node confirms it. Measured 2026-09-15
// on node 2: the single GridSync sent after a login was lost and nothing re-sent
// it — the node sat in Mode A for 13 minutes with Timed Mode ON.
// ---------------------------------------------------------------------------

namespace {
// handle_command_ack_ is protected. A probe that adds no state, so the harness's
// listener can be addressed through it — the node's CommandAck reaches exactly
// this function in production (set_response -> handle_command_ack_).
struct AckProbe : LORAClient {
    using esphome::lora_tracker::LORAListener::handle_command_ack_;
};
void deliverAck(LORAClient &rol, uint32_t msgid) {
    static_cast<AckProbe &>(rol).handle_command_ack_(msgid);
}
}  // namespace

TEST(RealLoraClient, AnUnconfirmedGridSyncIsRepublishedUntilTheNodeAcksIt) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.rol.gridSyncAwaitingAck());
    const uint32_t first = h.rol.gridsync_msgids_[0];

    h.clock.tick(esphome::lora_tracker::LORAListener::kGridSyncRetryMs + 1);
    EXPECT_EQ(h.rol.gridSyncRepublishes(), 1u) << "no confirmation: published again";
    EXPECT_TRUE(h.rol.gridSyncAwaitingAck());

    deliverAck(h.rol, first);   // a late ack for the FIRST publish still counts
    EXPECT_FALSE(h.rol.gridSyncAwaitingAck());
    h.clock.tick(5 * esphome::lora_tracker::LORAListener::kGridSyncRetryMs);
    EXPECT_EQ(h.rol.gridSyncRepublishes(), 1u) << "confirmed: no more re-publishes";
}

TEST(RealLoraClient, AnAckForAnotherFrameDoesNotConfirmTheGrid) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.rol.gridSyncAwaitingAck());
    deliverAck(h.rol, h.rol.gridsync_msgids_[0] + 100);
    EXPECT_TRUE(h.rol.gridSyncAwaitingAck())
        << "only an ack naming a GridSync's own msgid confirms the grid";
}

TEST(RealLoraClient, GridSyncRepublishingIsBounded) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);
    for (int i = 0; i < 20; ++i)
        h.clock.tick(esphome::lora_tracker::LORAListener::kGridSyncRetryMs + 1);
    EXPECT_EQ(h.rol.gridSyncRepublishes(),
              esphome::lora_tracker::LORAListener::kGridSyncMaxRepublishes)
        << "a node that never answers must not cost a burst every 6 s forever";
    EXPECT_FALSE(h.rol.gridSyncAwaitingAck());
}

// ---------------------------------------------------------------------------
// Placement asks the INTERVAL question, not the high-water one.
//
// nextPlacementT0_ is the production path: every placed downlink comes through
// send_aligned_ into it. It used to call the two-argument
// nextClearT0ForSlotUs, which floors the search at the END of the latest
// reservation — so a short frame whose air was free BEFORE a later burst was
// pushed past that burst instead of being placed in the hole. Measured
// 2026-09-15: two of node 2's marks never sent around a beacon, two empty
// windows. At the time that was one short of demotion — three empty windows
// tripped kMaxMissedMarks and dropped the node to Mode A's three windows per
// round plus the promotion trial's one instead of Mode B's single window, so
// the hub was causing the very receive-duty cost Mode B exists to avoid. That
// demotion was retired on 2026-09-20; the fix now buys latency instead.
//
// The five-argument form was written to replace it and says so in its own
// comment; only the ModeTest mark scheduler had ever been converted.
// ---------------------------------------------------------------------------

// NO CLIENT-SIDE WITNESS HERE, deliberately (2026-09-20).
//
// A test was written at this point asserting that nextPlacementT0_ returns a
// free mark before a later reservation. It was UNFALSIFIABLE and has been
// removed: this file compiles against the SHIM tracker, whose five-argument
// nextClearT0ForSlotUs forwards straight to the two-argument one
// (shims/.../lora_tracker.cpp:113-116), discarding copies, len and
// expects_reply. Both forms therefore return the same answer here, so the test
// passed with the fix present OR absent — a mutant reverting the call site
// SURVIVED against it, which is how this was caught.
//
// The SEMANTICS are covered where the real LORATracker is compiled:
// RealTrackerDefer.AMarkBeforeAQueuedBeaconIsClear queues a beacon at a later
// mark and asserts the earlier mark is still chosen — "the air before the
// beacon is free; placing after its END skipped marks". That is exactly the
// interval-vs-high-water distinction, and it passes.
//
// What remains unverified by any test is the WIRING — that nextPlacementT0_
// passes the shape through rather than asking the two-argument question. The
// signature change makes that a compile-time property, and adding a call
// recorder to the shim to assert it would be more machinery than the one-line
// call site is worth.

// ---------------------------------------------------------------------------
// A node asks for its grid again. Measured 2026-09-15 on node 2 (fw 1.0.92): a
// demoted node that kept its grid came back 11 ms off its marks after ~17 minutes
// without a beacon.
// ---------------------------------------------------------------------------

namespace {
struct SyncRequestProbe : LORAClient {
    using esphome::lora_tracker::LORAListener::handle_grid_sync_request_;
};
void deliverSyncRequest(LORAClient &rol) {
    GridSyncRequest r = GRID_SYNC_REQUEST__INIT;
    r.reason          = (uint32_t) timedmode::SyncRequestReason::AnchorStale;
    r.ssinceanchorfix = 1200;
    static_cast<SyncRequestProbe &>(rol).handle_grid_sync_request_(&r);
}
// Timed mode on and the first GridSync confirmed, as a node on its grid has it.
void confirmedGrid(real_helpers::RealHubHarness &h) {
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.rol.gridSyncAwaitingAck());
    deliverAck(h.rol, h.rol.gridsync_msgids_[0]);
    ASSERT_FALSE(h.rol.gridSyncAwaitingAck());
}
}  // namespace

// THE SWITCH MUST ACTUALLY REACH THE LADDER (2026-09-21).
//
// timed_mode_policy_test has four witnesses for optimistic single shot, and
// none of them can see this: they build a HubBelief by hand and call
// txRefusalFor directly, so they would pass unchanged if
// enable_optimistic_single_shot() wrote to a field nothing reads. That is the
// shape of the two fixes this session that a green suite certified and hardware
// proved inert.
//
// This drives the PRODUCTION path instead: setter -> optimistic_single_shot_ ->
// hubBeliefNow_()'s stamp -> txRefusalNow(). The preference is stamped in
// hubBeliefNow_() rather than stored in belief_ on purpose — belief_ holds only
// what the hub has OBSERVED, and an operator's choice is not an observation.
TEST(RealLoraClient, TheOptimisticSwitchChangesThisListenersOwnDecision) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    confirmedGrid(h);

    // The hub only knows a firmware version once a decrypted beacon has carried
    // one (hubBeliefNow_: firmware_known = node_fw_version_ != 0), and
    // FirmwareUnknown is tested BEFORE either staleness rung. Without this the
    // ladder stops at refusal 5 and the staleness rungs are never reached —
    // which is exactly what the precondition below caught on the first run.
    h.rol.node_fw_version_ = 10104;   // 1.1.4

    // A node that reported a good phase: every rung passes and single shot is
    // granted, which is the only starting point from which "stale" is the ONLY
    // thing that can later refuse it.
    h.rol.notePhaseReportForTest(/*rtc_slow_src=*/2, /*err_us=*/500,
                                 /*spread_us=*/800, /*samples=*/8,
                                 /*outside_guard=*/0, /*node_timed_rx=*/true);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None)
        << "precondition: a fresh, good phase report grants single shot";

    // Age it past the hub's own published bound. Now the ONLY failing rung is
    // the staleness one — asserted, not assumed, so a setup that lands on some
    // other rung fails here instead of passing for the wrong reason.
    proto_sim_timer_advance_us((int64_t) 24 * 60 * 60 * 1'000'000LL);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::ConfirmationStale)
        << "precondition: staleness, and nothing else, is what now refuses";
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst);

    // THE ASSERTION: the switch, on the real listener, changes the real
    // decision. Default off, so this is also the proof the default was inert.
    ASSERT_FALSE(h.rol.optimistic_single_shot()) << "must default OFF";
    h.rol.enable_optimistic_single_shot(true);

    EXPECT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None)
        << "the switch must reach hubBeliefNow_() and the ladder — if this "
           "still reads ConfirmationStale the setter is writing to a field "
           "nothing reads, which is a switch that toggles nothing";
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    // ...and turning it back off restores the refusal, so the effect is the
    // switch's and not something latched once on the way through.
    h.rol.enable_optimistic_single_shot(false);
    EXPECT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::ConfirmationStale);
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst);
}

TEST(RealLoraClient, TheOptimisticSwitchStillBurstsOnPositiveEvidence) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    confirmedGrid(h);
    // FirmwareUnknown is tested before either staleness rung — see the sibling
    // test above. Without a version the ladder stops at refusal 5.
    h.rol.node_fw_version_ = 10104;   // 1.1.4
    h.rol.notePhaseReportForTest(2, 500, 800, 8, 0, true);
    proto_sim_timer_advance_us((int64_t) 24 * 60 * 60 * 1'000'000LL);
    h.rol.enable_optimistic_single_shot(true);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None)
        << "precondition: the switch has granted single shot on a stale belief";

    // Now withdraw the grid: positive evidence that the node is NOT on it. The
    // switch relaxes aged-out evidence only, never a fact the hub observed.
    h.rol.enable_timed_mode(false);
    EXPECT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::GridDisabled)
        << "optimism must never outrank something the hub actually knows";
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst);
}

namespace {
// handle_beacon_ is protected, like handle_command_ack_ and
// handle_grid_sync_request_ above. Same probe pattern, and for the same reason:
// the node's wake beacon reaches exactly this function in production, so a test
// that goes through it is testing the production path rather than a hook.
struct BeaconProbe : LORAClient {
    using esphome::lora_tracker::LORAListener::handle_beacon_;
};

// One wake beacon carrying the PREVIOUS wake's Class A funnel.
//
// Only the fields the ledger reads are set; the rest stay at INIT defaults.
// prevbeaconmsgid is the wake's identity — the ledger keys on it so a wake
// reported twice is counted once.
void deliverBeaconWithWakeFunnel(LORAClient &rol, uint32_t prev_msgid,
                                 uint32_t windows, uint32_t hits,
                                 uint32_t detected, uint32_t crc) {
    NodeWakeBeacon b     = NODE_WAKE_BEACON__INIT;
    b.reason             = (WakeReason) WAKE_REASON__WAKE_TIMER_CHECKIN;
    b.mode               = NODE_MODE__MODE_AUTO;
    b.prevbeaconmsgid    = prev_msgid;
    b.prevwakewindows    = windows;
    b.prevwakehits       = hits;
    b.prevwakedetected   = detected;
    b.prevwakecrcvalid   = crc;
    static_cast<BeaconProbe &>(rol).handle_beacon_(&b);
}
}  // namespace

// MODE C's CLASS A LEDGER (2026-09-22).
//
// Both halves of this measurement already existed and were never added up: the
// hub places one reply into RX1 on every check-in, and the node reports that
// wake's funnel in its NEXT beacon. Without the pair, an empty Class A window
// is ambiguous between "nothing was aimed at it" and "aimed and missed" — which
// is exactly why no Class A FER could be quoted from the 2026-09-21 Mode C run
// (`windows 4 hits 1`, `windows 2 hits 1`).
//
// offered is counted at the PLACEMENT, so it answers the question the funnel
// alone cannot: did the hub aim a frame at a window it believed open.
TEST(RealLoraClient, TheClassALedgerCountsOnlyFramesActuallyPlaced) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    ASSERT_EQ(h.rol.class_a_stats().offered, 0u) << "a fresh listener owes nothing";

    std::vector<uint8_t> payload{1, 2, 3, 4};

    // DECLINE 1 — the default. A node that has never said it is in AUTO is
    // MODE_INTERACTIVE and opens no Class A window at all.
    ASSERT_EQ(h.rol.node_mode_, (uint32_t) NODE_MODE__MODE_INTERACTIVE);
    ASSERT_EQ(h.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::NotClassA);
    EXPECT_EQ(h.rol.class_a_stats().offered, 0u);

    // DECLINE 2 — Class A, but the hub has never heard an uplink from this node,
    // so there is no origin to hang the window off.
    //
    // THIS is the rung the first version of this test claimed to exercise. It
    // called send_timesync() on a node left at the INTERACTIVE default, so it
    // stopped at DECLINE 1 and never reached here — a mutant that counted a
    // NoUplinkStamp decline as an offer SURVIVED it, as did deleting the
    // increment outright. The mode check is what the order of the ladder makes
    // load-bearing.
    h.rol.node_mode_ = (uint32_t) NODE_MODE__MODE_AUTO;
    ASSERT_EQ(h.rol.last_uplink_t0_us_, 0);
    ASSERT_EQ(h.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::NoUplinkStamp);
    EXPECT_EQ(h.rol.class_a_stats().offered, 0u)
        << "a declined placement is not an offer: counting it would inflate the "
           "denominator with frames that never went into a window";

    // PLACED — the node's own uplink gives the window an origin. Without this
    // half the test is one-sided: it proves the counter does not over-count
    // while saying nothing about whether it counts at all.
    constexpr int64_t kT0 = 100'000;
    h.tracker.last_rx_t0_us_v  = kT0;
    h.tracker.rx_uncertainty_v = 250;
    auto from_self = real_helpers::serialize_avail(/*sender=*/18, /*msg_id=*/1);
    h.rol.set_response(from_self.data(), from_self.size());
    ASSERT_EQ(h.rol.last_uplink_t0_us_, kT0)
        << "the node's OWN uplink is the origin — precondition, not the claim";

    ASSERT_EQ(h.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::Placed);
    EXPECT_EQ(h.rol.class_a_stats().offered, 1u)
        << "the hit rate divides by this, so the increment has to be pinned: "
           "deleting it left the earlier version of this test green";

    // DECLINE 3 — both windows gone. The node is asleep, and this is the rung a
    // dense Mode C campaign hits most often, not a corner case: every check-in
    // the hub answers late lands here. A mutant that counted it as an offer
    // survived the three-stage version of this test, which is the whole reason
    // this stage exists — an inflated denominator makes Class A look worse than
    // it is, and the funnel is what the mode is being judged on.
    proto_sim_timer_advance_us(3 * 1'000'000);  // past RX2 at t0 + 2 s
    ASSERT_EQ(h.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::WindowsPast);
    EXPECT_EQ(h.rol.class_a_stats().offered, 1u)
        << "a frame the hub declined to send because the node is asleep was "
           "never aimed at a window; counting it would inflate the denominator";
}

// Length re-encode check (FrameCanon.h), hub side. The hub's T0 stamp for an
// uplink comes from its on-air length, which the CMAC does not cover, so a
// non-canonical uplink is still PROCESSED (counter advanced, command run) but
// must not become the node's Class A window origin ("no usable origin" -> burst).
namespace {
// Splice `extra` inside the header field (field 1, one-byte length) of a packed frame.
std::vector<uint8_t> hdr_splice(const std::vector<uint8_t> &f, const std::vector<uint8_t> &extra) {
    EXPECT_EQ(f[0], 0x0A);
    const size_t hlen = f[1];
    std::vector<uint8_t> out{0x0A, (uint8_t)(hlen + extra.size())};
    out.insert(out.end(), f.begin() + 2, f.begin() + 2 + hlen);
    out.insert(out.end(), extra.begin(), extra.end());
    out.insert(out.end(), f.begin() + 2 + hlen, f.end());
    return out;
}
}  // namespace

TEST(RealLoraClient, ACanonicalUplinkBecomesTheClassAOriginAndTripsNothing) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.tracker.last_rx_t0_us_v = 100'000;
    auto f = real_helpers::serialize_avail(18, 1);
    h.rol.set_response(f.data(), f.size());
    EXPECT_TRUE(h.rol.rx_timing_trusted_);
    EXPECT_EQ(h.rol.noncanonical_frames_, 0u) << "a same-version uplink must never trip the check";
    EXPECT_EQ(h.rol.last_uplink_t0_us_, 100'000);
}

TEST(RealLoraClient, APaddedUplinkIsProcessedButNeverBecomesTheClassAOrigin) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.tracker.last_rx_t0_us_v = 100'000;

    // Duplicate msgId (field 4, tag 0x20) with the same value: identical decoded
    // content, longer wire, so the tracker's length-derived T0 is skewed.
    auto padded = hdr_splice(real_helpers::serialize_avail(18, 1), {0x20, 0x01});
    h.rol.set_response(padded.data(), padded.size());
    EXPECT_FALSE(h.rol.rx_timing_trusted_);
    EXPECT_EQ(h.rol.noncanonical_frames_, 1u);
    EXPECT_EQ(h.rol.last_uplink_msgid_, 1u) << "demote, do not drop: the frame still advanced the counter";
    EXPECT_EQ(h.rol.last_uplink_t0_us_, 0) << "but its skewed T0 must not become the origin";

    // A previously GOOD origin is withdrawn, not left stale, by a later padded frame.
    auto good = real_helpers::serialize_avail(18, 2);
    h.tracker.last_rx_t0_us_v = 200'000;
    h.rol.set_response(good.data(), good.size());
    ASSERT_EQ(h.rol.last_uplink_t0_us_, 200'000);
    auto padded2 = hdr_splice(real_helpers::serialize_avail(18, 3), {0x20, 0x03});
    h.tracker.last_rx_t0_us_v = 300'000;
    h.rol.set_response(padded2.data(), padded2.size());
    EXPECT_EQ(h.rol.last_uplink_t0_us_, 0);

    // The verdict is per frame: the next canonical uplink restores a usable origin.
    auto next = real_helpers::serialize_avail(18, 4);
    h.tracker.last_rx_t0_us_v = 400'000;
    h.rol.set_response(next.data(), next.size());
    EXPECT_TRUE(h.rol.rx_timing_trusted_);
    EXPECT_EQ(h.rol.last_uplink_t0_us_, 400'000);
}

TEST(RealLoraClient, AnUplinkWithAnUnknownFieldStillRunsButIsNotATimingSource) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.tracker.last_rx_t0_us_v = 100'000;

    // A NEWER node's extra field (forward compatibility): the lengths agree,
    // because protobuf-c counts unknown fields, but it sits outside the CMAC.
    auto fwd = real_helpers::serialize_avail(18, 1);
    fwd.push_back(0xF8); fwd.push_back(0x06); fwd.push_back(0x01);
    h.rol.set_response(fwd.data(), fwd.size());
    EXPECT_FALSE(h.rol.rx_timing_trusted_);
    EXPECT_EQ(h.rol.noncanonical_frames_, 1u);
    EXPECT_EQ(h.rol.last_uplink_msgid_, 1u) << "the frame is processed";
    EXPECT_EQ(h.rol.last_uplink_t0_us_, 0);

    // An unknown field INSIDE the header is caught the same way.
    h.tracker.last_rx_t0_us_v = 500'000;
    auto hdr_unk = hdr_splice(real_helpers::serialize_avail(18, 2), {0xF8, 0x06, 0x01});
    h.rol.set_response(hdr_unk.data(), hdr_unk.size());
    EXPECT_FALSE(h.rol.rx_timing_trusted_);
    EXPECT_EQ(h.rol.noncanonical_frames_, 2u);
    EXPECT_EQ(h.rol.last_uplink_t0_us_, 0);
}

TEST(RealLoraClient, TheClassALedgerAccumulatesTheNodesOwnFunnel) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    // Two wakes, each reporting the PREVIOUS wake's funnel, as the beacon does.
    deliverBeaconWithWakeFunnel(h.rol, /*prev_msgid=*/11,
                                /*windows=*/2, /*hits=*/1, /*detected=*/1, /*crc=*/1);
    deliverBeaconWithWakeFunnel(h.rol, /*prev_msgid=*/12,
                                /*windows=*/3, /*hits=*/2, /*detected=*/2, /*crc=*/2);

    const auto &s = h.rol.class_a_stats();
    EXPECT_EQ(s.windows,   5u);
    EXPECT_EQ(s.hits,      3u);
    EXPECT_EQ(s.detected,  3u);
    EXPECT_EQ(s.crc_valid, 3u) << "true FER is 1 - crcValid/detected, so both "
                                  "stages have to be carried, not just the total";
}

TEST(RealLoraClient, TheClassALedgerDoesNotDoubleCountOneWake) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    // THE GUARD THIS TEST EXISTS FOR. Each beacon reports the PREVIOUS wake, so
    // two beacons from one wake carry the SAME prevbeaconmsgid and the same
    // funnel. fw 1.1.2 and 1.1.3 both did exactly that — a MODE_CHANGED and a
    // TIMER_CHECKIN per wake — and the one-uplink fix in 1.1.4 is what stopped
    // it. The ledger must not depend on that fix holding.
    deliverBeaconWithWakeFunnel(h.rol, /*prev_msgid=*/21,
                                /*windows=*/2, /*hits=*/1, /*detected=*/1, /*crc=*/1);
    deliverBeaconWithWakeFunnel(h.rol, /*prev_msgid=*/21,
                                /*windows=*/2, /*hits=*/1, /*detected=*/1, /*crc=*/1);

    const auto &s = h.rol.class_a_stats();
    EXPECT_EQ(s.windows, 2u) << "the same wake, reported twice, is ONE wake — a "
                                "regression of the one-uplink fix must not "
                                "silently halve the apparent FER";
    EXPECT_EQ(s.hits,    1u);
}

TEST(RealLoraClient, ANodeAskingForItsGridIsSentItAgain) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    confirmedGrid(h);
    // esp_timer, not SimClock: the hub stamps a publish on esp_timer_get_time(), and
    // the gate counts whole seconds, so exactly 60 s after a 1 us stamp reads 59.
    proto_sim_timer_advance_us((int64_t) (timedmode::kHubSyncRequestMinIntervalS + 1) * 1'000'000);

    deliverSyncRequest(h.rol);
    EXPECT_TRUE(h.rol.gridSyncAwaitingAck())
        << "published again, and waiting for the node to confirm it like any GridSync";
    EXPECT_EQ(h.rol.gridSyncRequestsAnswered(), 1u);
}

TEST(RealLoraClient, ARequestWithinAMinuteOfAPublishIsNotAnswered) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    confirmedGrid(h);
    proto_sim_timer_advance_us((int64_t) (timedmode::kHubSyncRequestMinIntervalS - 1) * 1'000'000);

    deliverSyncRequest(h.rol);
    EXPECT_FALSE(h.rol.gridSyncAwaitingAck()) << "at most one burst a minute, however many ask";
    EXPECT_EQ(h.rol.gridSyncRequestsIgnored(), 1u);
}

TEST(RealLoraClient, ARequestWhileAGridSyncAwaitsItsAckIsNotAnswered) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.rol.gridSyncAwaitingAck());
    const uint8_t republishes = h.rol.gridSyncRepublishes();

    deliverSyncRequest(h.rol);
    EXPECT_EQ(h.rol.gridSyncRequestsIgnored(), 1u)
        << "the re-publish is already doing the job";
    EXPECT_EQ(h.rol.gridSyncRepublishes(), republishes) << "and the request adds nothing to it";
}

// ---------------------------------------------------------------------------
// Review finding 1 (2026-09-15): a plaintext uplink was acted on while the session
// was confirmed. The header is in the clear on every frame, so the msgid an attacker
// needs is not a secret, and dispatch is not inert — one admitted frame retires a
// command, confirms a push, or sets the single-shot belief.
// ---------------------------------------------------------------------------

namespace {
struct TrackedOpProbe : LORAClient {
    using esphome::lora_tracker::LORAListener::op_awaiting_ack_;
    using esphome::lora_tracker::LORAListener::op_last_msgid_;
};

std::vector<uint8_t> plaintext_ack_frame(uint32_t msgid, uint32_t ack_msg_id,
                                         bool timed_rx_active) {
    LoraHeader hdr       = LORA_HEADER__INIT;
    hdr.destaddress      = esphome::lora_tracker::kHubAddress;
    hdr.destsubnet       = 2;
    hdr.senderaddress    = 18;
    hdr.msgid            = msgid;

    PhaseReport pr       = PHASE_REPORT__INIT;
    pr.samples           = 12;
    pr.rtcslowsrc        = 2;          // crystal
    pr.timedrxactive     = timed_rx_active;

    CommandAck ack       = COMMAND_ACK__INIT;
    ack.ack_msg_id       = ack_msg_id;
    ack.status           = ACK_STATUS__ACK_OK;
    ack.phase            = &pr;

    LoraClientResponseMessage resp = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    resp.header          = &hdr;
    resp.proto_case      = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK;
    resp.ack             = &ack;

    std::vector<uint8_t> out(lora_client_response_message__get_packed_size(&resp));
    lora_client_response_message__pack(&resp, out.data());
    return out;
}
}  // namespace

TEST(RealLoraClient, AForgedPlaintextAckCannotRetireATrackedCommand) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_         = true;
    h.rol.session_confirmed_  = true;   // the node has proved it holds the key
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    auto &op = static_cast<TrackedOpProbe &>(h.rol);
    ASSERT_TRUE(op.op_awaiting_ack_) << "precondition: a command is in flight";

    // Just above the hub's replay counter: inside the 1024-wide window, which is
    // what an attacker reading the plaintext header would choose.
    auto forged = plaintext_ack_frame(/*msgid=*/5, op.op_last_msgid_, false);
    h.rol.set_response(forged.data(), forged.size());

    EXPECT_TRUE(op.op_awaiting_ack_)
        << "an unauthenticated ack must not retire a command the node may never have had";
    EXPECT_EQ(h.rol.plaintextRefused(), 1u);
}

TEST(RealLoraClient, AForgedPlaintextAckCannotEarnSingleShot) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_        = true;
    h.rol.session_confirmed_ = true;

    auto forged = plaintext_ack_frame(/*msgid=*/5, /*ack_msg_id=*/1, /*timed=*/true);
    h.rol.set_response(forged.data(), forged.size());

    EXPECT_FALSE(h.rol.hubBelief().phase_reported)
        << "the phase report is evidence only when the frame carrying it was authenticated";
    EXPECT_FALSE(h.rol.hubBelief().node_timed_rx);
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst);
}

TEST(RealLoraClient, PlaintextIsStillTheBootstrapBeforeASessionExists) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;           // no session confirmed: the node holds no key

    auto frame = plaintext_ack_frame(/*msgid=*/5, /*ack_msg_id=*/1, /*timed=*/false);
    h.rol.set_response(frame.data(), frame.size());

    EXPECT_EQ(h.rol.plaintextRefused(), 0u) << "refusing here would close the bootstrap";
    EXPECT_EQ(h.rol.frame_counter_.rx_message_id, 5u)
        << "and with nothing to authenticate with, the msgid is the only sequencing there is";
}

TEST(RealLoraClient, ARequestWithTimedModeOffIsNotAnswered) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    deliverSyncRequest(h.rol);
    EXPECT_FALSE(h.rol.gridSyncAwaitingAck());
    EXPECT_EQ(h.rol.gridSyncRequestsIgnored(), 1u);
}

// Real-code A1: REGISTER → real LORAListener sends ClientConfig with the
// listener's address and the matching MAC, then schedules its 500 ms
// login_startup timer.
TEST(RealLoraClient, RegisterTriggersClientConfigAndLoginTimer) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};

    // An UNPROVISIONED node: it holds no session, so plaintext is the only way
    // to reach it and accepting it is the bootstrap. This is the one case where
    // the config goes out immediately, in the clear.
    auto reg = serialize_register(kMacRol2, /*needs_config=*/true);
    h.rol.set_response(reg.data(), reg.size());

    EXPECT_TRUE(h.rol.registered_)
        << "REGISTER must flip registered_ true in real code.";

    int cfg_count = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (!m) continue;
        if (m->cmd == proto_sim::LoraClientOperationMessage::Cmd::ClientConfig) {
            EXPECT_EQ(m->clientconfig.addr, 18u);
            EXPECT_EQ(m->clientconfig.mac_addr, kMacRol2);
            ++cfg_count;
        }
    }
    EXPECT_EQ(cfg_count, 1)
        << "Real send_remote_config must emit exactly one ClientConfig.";
}

// Real-code C1: enter_sleep emits CMD_SLEEP and arms the fallback timer.
TEST(RealLoraClient, EnterSleepEmitsSleepCmdAndArmsFallback) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    h.rol.enterSleep();

    int sleep_msgs = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Sysop &&
            m->sysop == proto_sim::ClientOperation::CMD_SLEEP) ++sleep_msgs;
    }
    EXPECT_EQ(sleep_msgs, 1)
        << "Real enterSleep() must transmit CMD_SLEEP exactly once.";
}

// Real-code: triggerOTA emits CMD_OTA.
TEST(RealLoraClient, TriggerOtaEmitsOtaSysop) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    h.rol.triggerOTA();

    int ota = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Sysop &&
            m->sysop == proto_sim::ClientOperation::CMD_OTA) ++ota;
    }
    EXPECT_EQ(ota, 1)
        << "Real triggerOTA() must transmit CMD_OTA exactly once.";
}

// Real-code C3: enter_sleep arms a fallback timer; if no REGISTER ever
// arrives, the fallback fires and produces a LoginMsg.
TEST(RealLoraClient, SleepFallbackFiresWhenRegisterLost) {
    using namespace real_helpers;
    // Use a short sleep_duration so the virtual tick stays reasonable.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(60);  // 60 s → 119 s fallback (60 + 5 + 18*3)
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    rol.enterSleep();
    const int before = static_cast<int>(radio.transcript().size());

    constexpr uint32_t kExpected = 60u * 1000u + 5000u + 18u * 3000u;
    clock.tick(kExpected + 1000);

    int login_count = 0;
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            ++login_count;
    }
    EXPECT_GE(login_count, 1)
        << "Real enterSleep() fallback must fire a LoginMsg after the "
           "sleep+boot+stagger window if no REGISTER cancelled it.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Real-code C2: enter_sleep arms a fallback, REGISTER arrives during the
// wake window, fallback is cancelled, and only the REGISTER-path LoginMsg
// goes out (not the fallback one too).
TEST(RealLoraClient, EarlyRegisterCancelsSleepFallback) {
    using namespace real_helpers;
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(60);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    // ACK the login challenge. Without an ACK the hub legitimately RETRIES on
    // its exponential backoff (kLoginRetryBaseMs = 5 s, doubling), which puts
    // extra LoginMsgs on the wire and masks what this test is actually about:
    // whether the stale enterSleep() fallback timer was cancelled.
    ensure_psa_ready();
    attach_encrypted_login_ack(radio, rol, /*node_addr=*/18, /*subnet=*/2);

    rol.enterSleep();
    const int before = static_cast<int>(radio.transcript().size());

    // Node wakes early at 50 s and sends REGISTER.
    clock.tick(50'000);
    auto reg = serialize_register(kMacRol2);
    rol.set_response(reg.data(), reg.size());

    // REGISTER schedules the login challenge kRegisterToLoginDelayMs later.
    clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);

    int login_after_register = 0;
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            ++login_after_register;
    }
    EXPECT_EQ(login_after_register, 1)
        << "REGISTER-path login fired once.";

    // Tick well past the original fallback time. NO second login.
    clock.tick(300'000);
    int login_total = 0;
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            ++login_total;
    }
    EXPECT_EQ(login_total, 1)
        << "Real enterSleep() fallback timer must have been cancelled by "
           "the REGISTER handler. If a second LoginMsg shows up here, the "
           "stale-interval race is live.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Real-code D2 sibling: cross-listener isolation. A reply from sender=18
// must NOT advance rx on rol_1 (short_address=17). Two real LORAListener
// instances share the same tracker/radio/nonces.
TEST(RealLoraClient, ReplyFromOtherNodeRejectedByListener) {
    using namespace real_helpers;
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol_1, rol_2;
    rol_1.set_name("rol_1");
    rol_1.set_short_address(17);
    rol_1.set_subnet_address(2);
    rol_1.set_sleep_duration(21600);
    rol_1.set_address(0xE08CFE5FB7A4ULL);

    rol_2.set_name("rol_2");
    rol_2.set_short_address(18);
    rol_2.set_subnet_address(2);
    rol_2.set_sleep_duration(21600);
    rol_2.set_address(kMacRol2);

    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol_1.set_time(&time);
    rol_2.set_time(&time);
    tracker.register_client(&rol_1);
    tracker.register_client(&rol_2);
    rol_1.registered_ = true;
    rol_2.registered_ = true;

    // Synthesize a reply with sender=18. Inject into both listeners and
    // verify rol_1 ignores it (sender filter) while rol_2 acts on it.
    auto ack = serialize_avail(/*sender=*/18, /*msg_id=*/1);
    rol_1.set_response(ack.data(), ack.size());
    rol_2.set_response(ack.data(), ack.size());

    EXPECT_EQ(rol_1.frame_counter_.rx_message_id, 0u)
        << "rol_1 (short_address=17) must REJECT a reply from sender=18 — "
           "cross-listener filter is what keeps multi-node deployments "
           "from cross-talk.";
    EXPECT_EQ(rol_2.frame_counter_.rx_message_id, 1u)
        << "rol_2 must have accepted the reply matching its own address.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Real-code A2 sibling: REGISTER with wrong MAC is ignored — listener
// stays unregistered and emits no ClientConfig.
TEST(RealLoraClient, WrongMacRegisterIgnored) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};

    auto reg = serialize_register(0xAABBCCDDEEFFULL);  // wrong MAC
    h.rol.set_response(reg.data(), reg.size());

    EXPECT_FALSE(h.rol.registered_)
        << "Real REGISTER handler must reject non-matching MAC.";
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        EXPECT_TRUE(!m || m->cmd != proto_sim::LoraClientOperationMessage::Cmd::ClientConfig)
            << "Real code must not emit ClientConfig for unknown MAC.";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// P1 — the hub pushes wall-clock time to the node once the encrypted session is
// confirmed. The node has no clock source of its own, so this is the only way
// it ever learns the time; everything the scheduler will later do rests on it.
// ---------------------------------------------------------------------------
namespace {

// Decrypt a hub->node frame that was encrypted for `node_addr` with
// `base_nonce`, and return the inner (payload-only) operation message.
std::optional<proto_sim::LoraClientOperationMessage>
decrypt_downlink(const proto_sim::AirFrame& f, uint32_t base_nonce) {
    auto outer = proto_sim::as_op(f);
    if (!outer || outer->cmd != proto_sim::LoraClientOperationMessage::Cmd::Encrypted)
        return std::nullopt;

    // Tier 3: Encrypt-then-CMAC. node_addr is the destination (this is a
    // downlink TO the node); node_nonce is the fixed value every
    // session-establishing helper in this file used.
    framecrypto::EtmHeaderFields fields{};
    fields.downlink       = true;
    fields.session_id     = base_nonce;
    fields.dest_address   = outer->header.destAddress;
    fields.dest_subnet    = outer->header.destSubnet;
    fields.sender_address = outer->header.senderAddress;
    fields.msgid          = outer->header.msgId;
    fields.burst_index    = outer->header.burstIndex;
    fields.burst_count    = outer->header.burstCount;
    fields.on_mark        = outer->header.onMark;
    fields.fire_stamped   = outer->header.fireStamped;
    fields.fire_round     = outer->header.fireRound;
    fields.fire_offset_us = outer->header.fireOffsetUs;

    auto plain = proto_sim::encrypt_then_cmac_open(
        base_nonce, kTestNodeNonce, esphome::lora_tracker::kHubAddress,
        static_cast<uint8_t>(outer->header.destAddress), /*downlink=*/true, fields,
        outer->encrypted.ciphertext.data(), outer->encrypted.ciphertext.size(),
        outer->encrypted.tag.data(), outer->encrypted.tag.size());
    if (!plain) return std::nullopt;
    return proto_sim::deserialize_op(plain->data(), plain->size());
}

// Tier 3: seal an uplink the way the node would, post-session-establishment
// (sessionNonce stays 0 — only the session-OPENING uplink carries it, and
// every test using this has already gone through attach_encrypted_login_ack/
// drive_session for that). Collects the boilerplate every "build a real
// encrypted uplink by hand" test in this file repeats.
proto_sim::EtmResult seal_uplink_like_node(uint32_t base_nonce,
                                           const proto_sim::LoraHeader &header,
                                           const std::vector<uint8_t> &plain) {
    framecrypto::EtmHeaderFields fields{};
    fields.downlink       = false;
    fields.session_id     = base_nonce;
    fields.dest_address   = header.destAddress;
    fields.dest_subnet    = header.destSubnet;
    fields.sender_address = header.senderAddress;
    fields.msgid          = header.msgId;
    fields.burst_index    = header.burstIndex;
    fields.burst_count    = header.burstCount;
    fields.on_mark        = header.onMark;
    fields.fire_stamped   = header.fireStamped;
    fields.fire_round     = header.fireRound;
    fields.fire_offset_us = header.fireOffsetUs;
    return proto_sim::encrypt_then_cmac_seal(
        base_nonce, kTestNodeNonce, esphome::lora_tracker::kHubAddress,
        static_cast<uint8_t>(header.senderAddress), /*downlink=*/false, fields,
        plain.data(), plain.size());
}

// Drive REGISTER -> login -> encrypted ACK, leaving the session confirmed.
// Returns the base nonce the hub minted, so the caller can decrypt downlinks.
uint32_t drive_session(proto_sim::SimClock& clock, proto_sim::SimRadio& radio,
                       LORAClient& listener) {
    uint32_t captured_nonce = 0;
    radio.add_sink([&captured_nonce](const proto_sim::AirFrame& f) {
        if (f.dir != proto_sim::AirFrame::Dir::HubToNode) return;
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            captured_nonce = m->login.nonce;
    });
    attach_encrypted_login_ack(radio, listener, /*node_addr=*/18, /*subnet=*/2);

    auto reg = real_helpers::serialize_register(kMacRol2);
    listener.set_response(reg.data(), reg.size());
    clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    return captured_nonce;
}

} // namespace

TEST(RealLoraClient, TimeSyncPushedAfterSessionConfirmed) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    constexpr std::time_t kHubEpoch = 1787000000;
    esphome::ESPTime::set_timezone_offset(7200);   // CEST

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(kHubEpoch, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.login_acked_) << "session must be confirmed before TimeSync is due";

    const size_t before = radio.transcript().size();
    clock.tick(1000);   // past the 750 ms deferred push

    int found = 0;
    proto_sim::TimeSync got{};
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto inner = decrypt_downlink(radio.transcript()[i], base);
        if (inner && inner->cmd == proto_sim::LoraClientOperationMessage::Cmd::TimeSync) {
            ++found;
            got = inner->timesync;
        }
    }

    ASSERT_EQ(found, 1) << "exactly one TimeSync must follow session confirmation";
    EXPECT_EQ(got.epoch,     static_cast<uint64_t>(kHubEpoch));
    EXPECT_EQ(got.utcOffset, 7200);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Invariant I3 (mac-separation-implementation-plan.md section 2(b)):
// sealBurstCopyTag() must refuse to retag a copy whose ciphertext was
// sealed under a session that is no longer current — sealing it anyway
// would pair an OLD ciphertext with a NEW tag, silently wasting the copy
// (the node's CMAC check fails) rather than visibly dropping it here.
//
// The real queue/burst path retags synchronously inside the same call that
// seals (both the shim and the real tracker), so there is no window to
// inject a re-login between the two through the public send APIs. This
// calls sealBurstCopyTag() directly instead, after s_pack_operation_message
// has snapshotted session #1's key id and a second LOGIN has moved the
// listener on to session #2 — exactly the ordering a re-login mid-burst
// would produce.
TEST(RealLoraClient, StaleSessionCopyIsRefusedNotRetaggedUnderTheWrongKey) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    drive_session(clock, radio, rol);
    ASSERT_EQ(rol.sessionGenerationForTest(), 1u);
    rol.mark_session_confirmed_for_test();

    // Seal a real frame under session #1 — this is what snapshots
    // s_seal_key_id_map[18]. What happens to the packed bytes afterwards
    // (the shim retags and "sends" them immediately) doesn't matter here.
    rol.send_remote_config();

    // A second session, moving the listener on to a brand-new K_enc/K_mac
    // WITHOUT ever touching s_seal_key_id_map[18] — exactly like a real
    // frame still sitting in the tracker's queue when a re-login lands.
    // Via the test hook, not a real second LOGIN: the shim retags
    // synchronously inside send(), so there is no window between seal and
    // retag to drive a real re-login through anyway (see the banner above).
    ASSERT_TRUE(rol.deriveSessionKeysForTest(/*session_id=*/0xDEADBEEFu, kTestNodeNonce));
    ASSERT_EQ(rol.sessionGenerationForTest(), 2u)
        << "precondition: the second session must have derived NEW keys";

    LoraHeader hdr   = LORA_HEADER__INIT;
    hdr.destaddress   = 18;
    hdr.destsubnet    = 2;
    hdr.senderaddress = esphome::lora_tracker::kHubAddress;
    hdr.msgid         = 999;
    uint8_t ciphertext[4] = {0, 0, 0, 0};
    uint8_t tag[framecrypto::kSessionCmacTagBytes] = {0};
    EncryptedPayload enc = ENCRYPTED_PAYLOAD__INIT;
    enc.ciphertext.data = ciphertext;
    enc.ciphertext.len  = sizeof(ciphertext);
    enc.tag.data        = tag;
    enc.tag.len         = sizeof(tag);

    EXPECT_FALSE(rol.sealBurstCopyTag(&enc, &hdr))
        << "a copy sealed under session #1 must not be retagged under #2's key";
    EXPECT_EQ(rol.staleSessionDropsForTest(), 1u);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AProvisionedNodesConfigPushWaitsForEncryption) {
    // send_remote_config() packed raw, so ClientConfig ALWAYS went out in the
    // clear — including to a provisioned node, which refuses plaintext commands
    // because ClientConfig sets its address, subnet, name and sleep duration and
    // an unauthenticated frame must not be able to re-address a node. The hub
    // sent it anyway on the first REGISTER after every hub boot, set
    // config_synced_ = true regardless, and never retried: a config change
    // flashed into the hub was lost with nothing logged as a failure.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    // A PROVISIONED node re-registering after a hub reboot: needs_config false,
    // but this hub boot has not pushed yet.
    auto reg = real_helpers::serialize_register(kMacRol2, /*needs_config=*/false);
    rol.set_response(reg.data(), reg.size());

    auto count_plaintext_config = [&]() {
        int n = 0;
        for (const auto& f : radio.hub_to_node_frames()) {
            auto m = proto_sim::as_op(f);
            if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::ClientConfig)
                ++n;
        }
        return n;
    };
    EXPECT_EQ(count_plaintext_config(), 0)
        << "a provisioned node would drop this; sending it is wasted airtime "
           "and the config change is lost";
    EXPECT_FALSE(rol.config_synced_)
        << "a push that did not happen must not be recorded as one that did";

    // Now the session comes up. That is the first moment the config CAN be
    // encrypted, and it must go then.
    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_);

    EXPECT_TRUE(rol.config_synced_)
        << "the deferred push must actually happen once the session exists";
    EXPECT_EQ(count_plaintext_config(), 0)
        << "and it must go out ENCRYPTED — a readable ClientConfig on the air "
           "is one the node refuses";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AProvisionedNodesCoverConfigIsEncryptedToo) {
    // The ClientConfig half of the deferred push was fixed to go through
    // s_pack_operation_message; the CoverConfig half was not. The deferred push
    // replays the stored REGISTER into every child node, LoraCoverComponent
    // answers a REGISTER with send_remote_config(), and that packed raw — so a
    // CoverConfig went out in the clear on a CONFIRMED session, and the node's
    // plaintext gate refused it ("Rejecting PLAINTEXT command (cmd_case=13)").
    // Measured 2026-09-26 on node 2 after a hub restart. The cover geometry
    // (durations, slack) never reached a node that had already provisioned, and
    // config_synced_ said it had.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    esphome::loracov::LoraCoverComponent cover;
    cover.set_name("rol_cover");
    cover.set_open_duration(60);
    cover.set_close_duration(61);
    cover.set_invert_position(false);
    cover.set_blind_height_mm(2000.0f);
    cover.set_axle_diameter_mm(60.0f);
    cover.set_blind_thickness_mm(8.0f);
    cover.setup();
    rol.register_lora_node(&cover);

    // A provisioned node re-registering after a hub reboot: no config wanted now.
    auto reg = real_helpers::serialize_register(kMacRol2, /*needs_config=*/false);
    rol.set_response(reg.data(), reg.size());

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_)
        << "the plaintext-on-a-confirmed-session case needs a confirmed session";
    ASSERT_TRUE(rol.config_synced_) << "the deferred push must have run";

    int plaintext_cover_config = 0;
    int encrypted_cover_config = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto plain = proto_sim::as_op(f);
        if (plain && plain->cmd == proto_sim::LoraClientOperationMessage::Cmd::CoverConfig)
            ++plaintext_cover_config;
        auto inner = decrypt_downlink(f, base);
        if (inner && inner->cmd == proto_sim::LoraClientOperationMessage::Cmd::CoverConfig) {
            ++encrypted_cover_config;
            EXPECT_EQ(inner->coverconfig.closeTime, 61u);
        }
    }
    EXPECT_EQ(plaintext_cover_config, 0)
        << "a CoverConfig readable on the air is one a provisioned node refuses";
    EXPECT_EQ(encrypted_cover_config, 1)
        << "and it must actually be delivered, encrypted, once the session exists";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, ADecryptedBeaconIsWhatCarriesThePhaseReport) {
    // The wiring, end to end, with no test hook anywhere in it.
    //
    // Every other §4.6 test seeds the report through notePhaseReportForTest so
    // it can exercise the POLICY transitions concisely. That is only legitimate
    // if something pins that handle_beacon_ really calls the same entry point —
    // otherwise the policy is reachable from tests and from nothing else, which
    // is the exact failure this whole exercise has been about.
    //
    // It also pins the property that makes this evidence worth trusting: the
    // report arrives ENCRYPTED. handle_beacon_ runs only for a frame that
    // passed its GCM tag, so an attacker in radio range cannot hand the hub a
    // flattering phase report and talk it into single-shot.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_);

    rol.enable_timed_mode(true);
    EXPECT_FALSE(rol.hubBelief().phase_reported)
        << "nothing has reported a phase yet";

    // A real beacon, encrypted the way the node sends one.
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rol.frame_counter_.rx_message_id + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Beacon;
    inner.beacon.fwVersion     = 0x00010203;
    inner.beacon.phasePresent      = true;
    inner.beacon.phase.rtcSlowSrc   = 2;      // crystal
    inner.beacon.phase.errUs        = 120;
    inner.beacon.phase.spreadUs     = 300;
    inner.beacon.phase.samples      = timedmode::kPromotionPhaseSamples;
    inner.beacon.phase.outsideGuard = 0;
    inner.beacon.phase.timedRxActive = true;   // the node's own decision: in Mode B

    auto plain = proto_sim::serialize_resp_payload(inner);
    auto enc = seal_uplink_like_node(base, inner.header, plain);
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
    outer.encrypted.ciphertext = enc.ciphertext;
    auto frame = proto_sim::serialize_resp(outer);
    rol.set_response(frame.data(), frame.size());

    const auto b = rol.hubBelief();
    EXPECT_TRUE(b.phase_reported)
        << "handle_beacon_ must feed the phase report into the belief";
    EXPECT_EQ(b.phase_err_us, 120);
    EXPECT_EQ(b.phase_spread_us, 300);
    EXPECT_EQ(b.phase_samples, timedmode::kPromotionPhaseSamples);
    EXPECT_EQ(b.rtc_src, timedmode::RtcSlowSrc::Crystal);
    EXPECT_EQ(rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot)
        << "a healthy report from a real encrypted beacon must earn single-shot";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AForgedPlaintextUplinkMovesNeitherTheBeliefNorTheClassAOrigin) {
    // admit_frame_ checks the address and the replay window. Neither is
    // authentication, and both of C2's and section 4.6's inputs were taken
    // there — so anyone in radio range had two levers with no key at all:
    //
    //   * one out-of-slot frame per round resets in_slot_acks, denying the node
    //     single-shot for as long as the attacker keeps transmitting;
    //   * three frames timed AT the mark EARN single-shot, after which the hub
    //     sends one copy to a real node it has no evidence about.
    //
    // The measurement now hangs off the same rule as the replay counter: a
    // frame moves the hub's beliefs only once it has earned the right to.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_);
    rol.node_fw_version_ = 0x00010203;
    rol.enable_timed_mode(true);
    ASSERT_TRUE(tracker.gridStarted());

    const int64_t origin_before = rol.last_uplink_t0_us_;
    uint32_t msgid = rol.frame_counter_.rx_message_id;

    // Three PLAINTEXT uplinks placed perfectly at this node's mark — the exact
    // evidence section 4.6 promotes on, forged.
    for (int i = 0; i < 3; ++i) {
        const int64_t mark =
            tracker.nextT0ForSlotUs(rol.grid_slot(), tracker.gridAnchorUs());
        tracker.last_rx_t0_us_v = mark + (int64_t) LORAClient::kUplinkOffsetUs;
        auto forged = real_helpers::serialize_avail(/*sender=*/18, ++msgid);
        rol.set_response(forged.data(), forged.size());
    }

    EXPECT_EQ(rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "unauthenticated frames must not earn a node single-shot";
    EXPECT_EQ(rol.last_uplink_t0_us_, origin_before)
        << "and must not become this node's Class A window origin";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, APlaintextFrameOnAConfirmedSessionCannotBeReplayedWithoutLimit) {
    // Duplicate rejection used to be a SIDE EFFECT of admit_frame_ assigning the
    // replay counter: the frame moved rx_message_id, so the next copy failed the
    // "msgid > rx_message_id" window. Taking that assignment away — correctly,
    // so an unauthenticated frame cannot ratchet the counter — removed the
    // dedup with it and nothing replaced it. The SAME captured frame was then
    // admitted and dispatched without limit, and dispatch_payload_ is not
    // inert: a replayed CommandAck clears op_awaiting_ack_ every time,
    // suppressing the retry ladder while the hub reports the command delivered.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_) << "the replay only applies to a live session";

    const uint32_t rx = rol.frame_counter_.rx_message_id;
    auto forged = real_helpers::serialize_avail(/*sender=*/18, rx + 5);

    // The first copy is admitted — this path stays open by design, because a
    // plaintext status frame from a live node is still worth acting on.
    rol.set_response(forged.data(), forged.size());
    const int64_t first_seen = rol.last_uplink_t0_us_;

    // Move the tracker's stamp so a SECOND admission would be visible.
    tracker.last_rx_t0_us_v = first_seen + 777'000;

    for (int i = 0; i < 5; ++i)
        rol.set_response(forged.data(), forged.size());

    EXPECT_EQ(rol.frame_counter_.rx_message_id, rx)
        << "still no ratchet — that fix must not regress";
    EXPECT_EQ(rol.last_uplink_t0_us_, first_seen)
        << "a replayed plaintext frame must be dropped as a duplicate, not "
           "re-admitted without limit";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AForgedPlaintextUplinkCannotRatchetTheHubsReplayCounter) {
    // The hub's mirror of the node's counter ratchet.
    //
    // admit_frame_ accepts any forward jump inside (rx, rx + 1024] and used to
    // ASSIGN rx there, before a byte had been authenticated — and setRxMessageId
    // persists, so the damage outlived a reboot. Anyone in radio range can read
    // the plaintext LoraHeader off a real uplink and send one frame at
    // msgid + 1000 claiming to be the node. Every genuine uplink after that was
    // logged as "duplicate or old message ID" and dropped, so the hub saw the
    // node as silent: no acks, retries exhausted, session torn down. The forged
    // frame never had to be acted on.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_) << "the attack only applies to a live session";

    const uint32_t rx_after_login = rol.frame_counter_.rx_message_id;

    // One forged plaintext frame, near the top of the acceptance window.
    auto forged = real_helpers::serialize_avail(/*sender=*/18,
                                                /*msg_id=*/rx_after_login + 1000);
    rol.set_response(forged.data(), forged.size());

    EXPECT_EQ(rol.frame_counter_.rx_message_id, rx_after_login)
        << "an unauthenticated uplink must not move the hub's replay counter";

    // And the node's next real frame — the very next msgid — is still accepted.
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rx_after_login + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
    inner.avail.available      = true;

    auto plain = proto_sim::serialize_resp_payload(inner);
    auto enc = seal_uplink_like_node(base, inner.header, plain);
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
    outer.encrypted.ciphertext = enc.ciphertext;
    auto real_frame = proto_sim::serialize_resp(outer);
    rol.set_response(real_frame.data(), real_frame.size());

    EXPECT_EQ(rol.frame_counter_.rx_message_id, rx_after_login + 1)
        << "the node's genuine next uplink must still be accepted — the forged "
           "frame must not have wedged the link";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, RX1IsAimedAtThisNodesOwnUplinkNotTheTrackersLastFrame) {
    // C2's hub half. The node's Class A windows hang off ITS OWN uplink, so the
    // hub's reply has to be placed from the stamp of that node's uplink — and
    // the tracker's stamp is global: one radio, one variable, overwritten by
    // every frame from every node. send_into_class_a_window_() read it 750 ms after the
    // uplink that triggered the reply, so on a 32-node fleet the common case was
    // aiming one copy — no burst to save it — at a window derived from someone
    // else's transmit.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol_1, rol_2;
    rol_1.set_name("rol_1"); rol_1.set_short_address(17); rol_1.set_subnet_address(2);
    rol_2.set_name("rol_2"); rol_2.set_short_address(18); rol_2.set_subnet_address(2);
    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol_1.set_time(&time); rol_2.set_time(&time);
    tracker.register_client(&rol_1);
    tracker.register_client(&rol_2);
    rol_1.registered_ = true;
    rol_2.registered_ = true;

    // Node 17 transmits. The tracker stamps it; both listeners see the frame,
    // only rol_1 admits it.
    constexpr int64_t kT0Node17 = 100'000;
    tracker.last_rx_t0_us_v = kT0Node17;
    tracker.rx_uncertainty_v = 250;
    auto from_17 = real_helpers::serialize_avail(/*sender=*/17, /*msg_id=*/1);
    rol_1.set_response(from_17.data(), from_17.size());
    rol_2.set_response(from_17.data(), from_17.size());

    // Node 18 transmits 300 ms later, inside the window before the hub replies
    // to node 17. This is the frame that used to decide where node 17's reply
    // went.
    constexpr int64_t kT0Node18 = 400'000;
    tracker.last_rx_t0_us_v = kT0Node18;
    auto from_18 = real_helpers::serialize_avail(/*sender=*/18, /*msg_id=*/1);
    rol_1.set_response(from_18.data(), from_18.size());
    rol_2.set_response(from_18.data(), from_18.size());

    EXPECT_EQ(rol_1.last_uplink_t0_us_, kT0Node17)
        << "a frame from another node must not become this node's window origin";
    EXPECT_EQ(rol_2.last_uplink_t0_us_, kT0Node18);

    // A node the hub has never heard from is MODE_INTERACTIVE, and an
    // interactive node opens no Class A window at all — it sweeps a free-running
    // one. Aiming a single copy at a window that is not there is strictly worse
    // than the burst it replaces, so the hub must DECLINE and let the caller
    // fall back.
    std::vector<uint8_t> payload{1, 2, 3, 4};
    EXPECT_EQ(rol_1.node_mode_, (uint32_t) NODE_MODE__MODE_INTERACTIVE)
        << "the safe default: a node that has not said otherwise is not Class A";
    EXPECT_EQ(rol_1.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::NotClassA)
        << "an interactive node has no RX1 to aim at; the caller must burst";

    // Now the node tells us, in a beacon, that it is in AUTO. It is on no grid,
    // so Class A is exactly the mode it is in.
    rol_1.node_mode_ = (uint32_t) NODE_MODE__MODE_AUTO;
    ASSERT_EQ(rol_1.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::Placed);
    EXPECT_EQ(tracker.last_copies, 1)
        << "a burst is the opposite construction to a single placed copy";

    // The contract is NOT "earliest_us equals the expression send_into_rx1_
    // computes" — restating the code's own arithmetic back at it proves
    // nothing. It is that the frame's T0 lands inside the window the NODE
    // opens, which the node builds with classa::rx1OpenUs/rx1CloseUs off its
    // own uplink. earliest_us is a FIRE instant, so the T0 the node sees is
    // kPreambleToT0Us later; a producer that forgets that conversion puts the
    // frame 3136 us late and this is the assertion that catches it.
    const int64_t fire      = tracker.last_earliest_us;
    const int64_t seen_t0   = fire + (int64_t) loratiming::kDownlinkPreambleToT0Us;
    const int64_t win_open  = classa::rx1OpenUs(kT0Node17);
    const int64_t win_close = classa::rx1CloseUs(kT0Node17);
    EXPECT_GE(seen_t0, win_open)
        << "frame arrives before node 17's RX1 window opens";
    EXPECT_LT(seen_t0, win_close)
        << "frame arrives after node 17's RX1 window has closed";
    EXPECT_EQ(seen_t0, kT0Node17 + (int64_t) classa::kRx1DelayUs)
        << "and it lands on the design point, guard G inside the open edge — "
           "placed from node 17's uplink, not from node 18's";
    EXPECT_FALSE(tracker.last_on_mark)
        << "placed on the node's UPLINK, not on its grid mark: read as a phase "
           "sample it is hundreds of ms of error and latches the node out of "
           "Mode B";

    // The margin the design promises, stated so a future edit that eats it
    // fails here rather than in the field. The guard is measured to the first
    // CHIRP, not to T0: the node must already be listening a full G before the
    // preamble starts, which is why the window opens kArmLeadUs (= T_pre + G)
    // ahead of the expected T0 rather than G ahead of it.
    EXPECT_EQ(fire - win_open, (int64_t) timedgrid::kGuardUs)
        << "the frame's first chirp must sit a full guard inside the window";
    EXPECT_EQ(seen_t0 - win_open, (int64_t) classa::kArmLeadUs);

    // And the late side: what is left between T0 and the window closing. This
    // is the budget the hub's own stamp uncertainty is spent against, so a
    // change that quietly halves it should fail here.
    EXPECT_EQ(win_close - seen_t0,
              (int64_t) timedgrid::kWindowUs - (int64_t) classa::kArmLeadUs);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// ---------------------------------------------------------------------------
// §4.6 — the hub's half: one copy instead of seventeen, once it has EVIDENCE
// ---------------------------------------------------------------------------
namespace {
namespace real_helpers {

// Feed the listener one uplink whose T0 lands exactly where the grid says this
// node transmits: its mark plus the uplink offset the hub itself publishes.
// The evidence §4.6 now promotes on: the node's own phase measurement, as it
// arrives from a decrypted beacon. Defaults are a healthy node.
void give_phase_report(RealHubHarness& h, int32_t err_us = 0,
                       int32_t spread_us = 0,
                       uint32_t samples = timedmode::kPromotionPhaseSamples,
                       uint32_t rtc = 2 /*crystal*/,
                       bool node_timed_rx = true /*the node reports it is in Mode B*/) {
    h.rol.notePhaseReportForTest(rtc, err_us, spread_us, samples, /*outside_guard=*/0,
                                 node_timed_rx);
}

void feed_in_slot_uplink(RealHubHarness& h, uint32_t msgid, int64_t err_us = 0) {
    const int64_t mark = h.tracker.nextT0ForSlotUs(h.rol.grid_slot(),
                                                   h.tracker.gridAnchorUs());
    h.tracker.last_rx_t0_us_v = mark + (int64_t) LORAClient::kUplinkOffsetUs + err_us;
    auto up = serialize_avail(/*sender=*/18, msgid);
    h.rol.set_response(up.data(), up.size());
}

}  // namespace real_helpers
}  // namespace

TEST(RealLoraClient, ARelogonMakesTheNodeEarnSingleShotFromScratch) {
    // §4.6's own rule: "a new session invalidates the hub's confidence... it
    // earns single-shot back from observation." send_login() set
    // session_changed and nothing else, but noteUplinkPlacement_ clears that
    // flag on the FIRST in-slot uplink — and in_slot_acks was still >=
    // kPromotionUplinks from the old session, so one lucky arrival restored
    // single-shot immediately. A node that just re-keyed after a reboot got a
    // one-copy command on the strength of evidence gathered before the reboot.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.tracker.gridStarted());

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot);

    h.rol.send_login();
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "a new session must not inherit the old session's confidence";

    // A report from the old session is not evidence about the new one, so the
    // node has to send a fresh beacon before single-shot returns.
    give_phase_report(h);
    EXPECT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot)
        << "a fresh report on the new session earns it back";
}

TEST(RealLoraClient, ARegisterMeansTheNodeRebootedAndConfidenceIsGone) {
    // rebooted_since_confirm is a txPolicyFor guard implementing rule 3, "any
    // hub uncertainty means burst". It defaulted true and was cleared by the
    // first in-slot uplink — and NOTHING ever set it again, so after the first
    // confirmation the guard was false for the life of the hub process and the
    // rule never fired. A REGISTER is the hub's actual notification that the
    // node restarted, which is exactly the uncertainty the flag names.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot);

    auto reg = serialize_register(kMacRol2);
    h.rol.set_response(reg.data(), reg.size());

    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "a node that just told us it rebooted has lost its grid phase";
}

// ---------------------------------------------------------------------------
// RX2 — the second window, which the hub never used.
//
// The node opens BOTH windows: classa::nextAction returns OpenRx2 whenever RX1
// passed with NO DATA, and that is exactly the state the hub leaves it in when
// its reply is not ready in time. The hub logged "RX1 already past" and fell
// back to a 17-copy burst — 1408 ms of air aimed at a node whose 29.44 ms
// window it could place exactly. `grep kRx2` in the hub returned nothing.
// ---------------------------------------------------------------------------
namespace {

// A registered listener whose Class A predicate passes, with the harness clock
// and radio wired up. Returns the tracker by reference so a test can read what
// was actually placed.
struct ClassANode {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker tracker;
    LORAClient  rol;

    ClassANode() {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        proto_sim_timer_reset();
        rol.set_name("rol");
        rol.set_short_address(17);
        rol.set_subnet_address(2);
        tracker.register_client(&rol);
        rol.registered_ = true;
        // What send_into_class_a_window_ requires: the node said AUTO in a
        // beacon we decrypted, and it is on no grid.
        rol.node_mode_ = (uint32_t) NODE_MODE__MODE_AUTO;
    }
    ~ClassANode() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

}  // namespace

TEST(RealLoraClient, AReplyTooLateForRx1GoesToRx2RatherThanToABurst) {
    ClassANode n;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;

    // Now is past RX1's fire instant and well before RX2's. Before this change
    // that was the "falling back" branch.
    proto_sim_timer_set_now_us(kT0Uplink + (int64_t) classa::kRx1DelayUs + 50'000);

    ASSERT_EQ(n.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::Placed)
        << "RX1 is gone, but RX2 is still ahead — that is a placeable window";
    EXPECT_EQ(n.tracker.last_copies, 1)
        << "a burst is the opposite construction to a single placed copy";

    // Same contract as the RX1 test: not "earliest_us equals the expression the
    // code computes", but "the T0 the NODE sees lands inside the window the
    // node opens", built from classa::rx2OpenUs/rx2CloseUs off its own uplink.
    const int64_t fire      = n.tracker.last_earliest_us;
    const int64_t seen_t0   = fire + (int64_t) loratiming::kDownlinkPreambleToT0Us;
    const int64_t win_open  = classa::rx2OpenUs(kT0Uplink);
    const int64_t win_close = classa::rx2CloseUs(kT0Uplink);
    EXPECT_GE(seen_t0, win_open)  << "frame arrives before RX2 opens";
    EXPECT_LT(seen_t0, win_close) << "frame arrives after RX2 has closed";
    EXPECT_EQ(seen_t0, kT0Uplink + (int64_t) classa::kRx2DelayUs)
        << "and on the design point, one RX2 delay after the node's own uplink";
    EXPECT_EQ(fire - win_open, (int64_t) timedgrid::kGuardUs)
        << "the first chirp must sit a full guard inside the window";
}

TEST(RealLoraClient, AWindowTooCloseToFireOnIsNotAWindow) {
    // popDue releases a placed frame kPrepareLeadUs early, so a target nearer
    // than that cannot be fired ON — asking for it puts the copy late by the
    // shortfall. RX1 half a millisecond away is unreachable, and the answer is
    // RX2 rather than the burst that used to be the only alternative.
    ClassANode n;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;

    const int64_t rx1_fire =
        loratiming::fireInstantUs(kT0Uplink + (int64_t) classa::kRx1DelayUs, 0);
    proto_sim_timer_set_now_us(rx1_fire - txqueue::kPrepareLeadUs / 10);

    ASSERT_EQ(n.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::Placed);
    const int64_t seen_t0 =
        n.tracker.last_earliest_us + (int64_t) loratiming::kDownlinkPreambleToT0Us;
    EXPECT_EQ(seen_t0, kT0Uplink + (int64_t) classa::kRx2DelayUs)
        << "an RX1 the queue cannot fire on must not be claimed as placed";
}

TEST(RealLoraClient, OnceBothWindowsArePastTheHubDeclines) {
    // What must not happen is a placed copy aimed at a window that has already
    // closed. The REASON matters as much as the refusal: this is a Class A
    // node the hub can place exactly, and the placement it computes is
    // "nowhere" — which is a different instruction to the caller than "I do
    // not know where this node listens".
    ClassANode n;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;
    proto_sim_timer_set_now_us(kT0Uplink + (int64_t) classa::kRx2DelayUs + 1);

    EXPECT_EQ(n.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::WindowsPast)
        << "both windows shut on a node the hub CAN place: it is asleep, and "
           "that must not be reported as an unplaceable node";
}

TEST(RealLoraClient, AnInteractiveNodeGetsNeitherWindow) {
    // The mode gate is checked before either window: an interactive node sweeps
    // a free-running window, so a single copy at a fixed offset hits it about
    // 6 % of the time while the 17-copy burst hits it every time. RX2 must not
    // become a second way to aim into silence.
    ClassANode n;
    n.rol.node_mode_ = (uint32_t) NODE_MODE__MODE_INTERACTIVE;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;
    proto_sim_timer_set_now_us(kT0Uplink + (int64_t) classa::kRx1DelayUs + 50'000);

    EXPECT_EQ(n.rol.send_into_class_a_window_(payload.data(), payload.size()),
              LORAClient::ClassAPlacement::NotClassA);
}

TEST(RealLoraClient, ADecryptedAckIsTheCarrierThatKeepsTheReportFresh) {
    // The beacon is not the carrier that makes §4.6 work — the ACK is.
    //
    // A wake beacon reports what the node knew at wake, which is nothing:
    // phase::Stats is a plain member, zeroed on every deep-sleep wake, and the
    // beacon goes out before that wake has heard a grid-aligned frame. The ack
    // answers the very command single-shot is decided for, and every addressed
    // frame feeds the phase tracker, so a node acking a command has just taken
    // a sample against the frame it is acking.
    //
    // The alternative was a periodic per-node uplink, which is what §4.4 prices
    // and rejects: a unicast keepalive at 5.8 min is 3.4x worse than plain
    // burst at 32 nodes.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    rol.enable_timed_mode(true);
    ASSERT_FALSE(rol.hubBelief().phase_reported);

    // First the wake beacon, exactly as a real one arrives: it carries the
    // firmware version the hub gates on, and a phase report of ZERO SAMPLES,
    // because phase::Stats is zeroed on every deep-sleep wake and the beacon
    // goes out before this wake has heard a grid-aligned frame. This is the
    // state §4.6 sat in: evidence that cannot arrive on the carrier that was
    // asked for it.
    {
        proto_sim::LoraClientResponseMessage bmsg;
        bmsg.header.destAddress   = esphome::lora_tracker::kHubAddress;
        bmsg.header.destSubnet    = 2;
        bmsg.header.senderAddress = 18;
        bmsg.header.msgId         = rol.frame_counter_.rx_message_id + 1;
        bmsg.proto                = proto_sim::LoraClientResponseMessage::Proto::Beacon;
        bmsg.beacon.fwVersion       = 0x00010203;
        bmsg.beacon.phasePresent    = true;
        bmsg.beacon.phase.rtcSlowSrc = 2;
        bmsg.beacon.phase.samples    = 0;      // nothing measured yet this wake

        auto bplain = proto_sim::serialize_resp_payload(bmsg);
        auto benc = seal_uplink_like_node(base, bmsg.header, bplain);
        proto_sim::LoraClientResponseMessage bouter;
        bouter.header               = bmsg.header;
        bouter.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
        bouter.encrypted.tag        = std::vector<uint8_t>(benc.tag, benc.tag + framecrypto::kSessionCmacTagBytes);
        bouter.encrypted.ciphertext = benc.ciphertext;
        auto bframe = proto_sim::serialize_resp(bouter);
        rol.set_response(bframe.data(), bframe.size());
    }
    EXPECT_FALSE(rol.hubBelief().phase_reported)
        << "zero samples is not a report, and must not read as a perfect one";
    EXPECT_EQ(rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "the wake beacon alone cannot promote — this is what was broken";

    // Then the ack, which is the carrier that can.
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rol.frame_counter_.rx_message_id + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Ack;
    inner.ack.ack_msg_id       = 7;
    inner.ack.phasePresent      = true;
    inner.ack.phase.rtcSlowSrc   = 2;      // crystal
    inner.ack.phase.errUs        = 90;
    inner.ack.phase.spreadUs     = 250;
    inner.ack.phase.samples      = timedmode::kPromotionPhaseSamples;
    inner.ack.phase.outsideGuard = 0;
    inner.ack.phase.timedRxActive = true;   // the node's own decision: in Mode B

    auto plain = proto_sim::serialize_resp_payload(inner);
    auto enc = seal_uplink_like_node(base, inner.header, plain);
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
    outer.encrypted.ciphertext = enc.ciphertext;
    auto frame = proto_sim::serialize_resp(outer);
    rol.set_response(frame.data(), frame.size());

    const auto b = rol.hubBelief();
    EXPECT_TRUE(b.phase_reported)
        << "the ack's phase report must reach the belief, not just the beacon's";
    EXPECT_EQ(b.phase_err_us, 90);
    EXPECT_EQ(b.phase_spread_us, 250);
    EXPECT_EQ(b.phase_samples, timedmode::kPromotionPhaseSamples);
    EXPECT_EQ(b.rtc_src, timedmode::RtcSlowSrc::Crystal);
    EXPECT_EQ(rol.txPolicyNow(), timedmode::TxPolicy::SingleShot)
        << "a healthy report on a real encrypted ack must earn single-shot";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AnAckWithNoPhaseReportLeavesTheBeliefAlone) {
    // ABSENT IS NOT EMPTY — the same rule the pending mask is built on. A node
    // whose firmware predates the field sends an ack with no PhaseReport, and
    // treating that as a report of zeros would silently demote a node the hub
    // has good evidence for. Let confirmation_age_s expire it instead.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    rol.enable_timed_mode(true);

    // Seed a good report, then send an ack that carries none.
    rol.notePhaseReportForTest(2, 100, 200, timedmode::kPromotionPhaseSamples);
    ASSERT_TRUE(rol.hubBelief().phase_reported);

    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rol.frame_counter_.rx_message_id + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Ack;
    inner.ack.ack_msg_id       = 9;
    inner.ack.phasePresent     = false;

    auto plain = proto_sim::serialize_resp_payload(inner);
    auto enc = seal_uplink_like_node(base, inner.header, plain);
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
    outer.encrypted.ciphertext = enc.ciphertext;
    auto frame = proto_sim::serialize_resp(outer);
    rol.set_response(frame.data(), frame.size());

    const auto b = rol.hubBelief();
    EXPECT_TRUE(b.phase_reported)   << "an absent report must not erase a good one";
    EXPECT_EQ(b.phase_err_us, 100);
    EXPECT_EQ(b.phase_samples, timedmode::kPromotionPhaseSamples);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, APhaseReportEarnsASingleCopyDownlink) {
    // TimedModePolicy.h's txPolicyFor() and HubBelief had NO production caller.
    // The entire airtime saving of Mode B is in that function — 17 copies down
    // to 1 — so a hub that never asked it paid Mode A's cost for Mode B's
    // narrower window, which is the worst of both.
    //
    // It then had a caller and still could not fire: promotion required three
    // consecutive uplinks observed in slot, and the node's transmit path cannot
    // produce them (CAD, a burst-end deferral and a 29-290 ms random backoff
    // stand in front of every uplink). The evidence is now the node's own phase
    // report — see TimedModePolicy.h for why that answers the question better,
    // not merely more conveniently.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    // Learned from a beacon in production; the policy refuses single-shot to a
    // node whose firmware it does not know.
    h.rol.node_fw_version_ = 0x00010203;

    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.tracker.gridStarted());

    // No report yet: a node that has told us nothing gets the burst.
    EXPECT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::Burst);

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot)
        << "phase inside the guard over enough samples, on the crystal, "
           "reported just now, firmware known";

    // And the belief has to reach the radio, which is the half that was missing.
    h.tracker.last_copies = 0;
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    EXPECT_EQ(h.tracker.last_copies, 1)
        << "the downlink must go out as ONE placed copy, not a 17-copy burst";
}

TEST(RealLoraClient, ANodeThatSaysItIsNotInModeBKeepsTheHubOnBursts) {
    // Decided 2026-09-14: the node is the authority on its own promotion. It
    // applies the phase test itself (enough samples, all inside the guard,
    // spread inside the guard, crystal) and reports the DECISION; the hub
    // follows it rather than re-judging the numbers.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot);

    // The node says it is not in Mode B: its window will not be where a single
    // copy is aimed.
    give_phase_report(h, 0, 0, timedmode::kPromotionPhaseSamples, /*rtc=*/2,
                      /*node_timed_rx=*/false);
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "a node that reports it is not arming timed windows must get bursts";
    EXPECT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::NodeNotTimed)
        << "and the published reason says it was the node's own decision";

    // Numbers the hub would once have refused no longer override the node:
    // the node applied its own test before it armed, and says it passed.
    give_phase_report(h, /*err_us=*/(int32_t) timedgrid::kGuardUs + 1000, 0,
                      timedmode::kPromotionPhaseSamples, /*rtc=*/2, /*node_timed_rx=*/true);
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot)
        << "the hub follows the node's decision, not a re-judgement of its numbers";

    give_phase_report(h, 0, 0, timedmode::kPromotionPhaseSamples, /*rtc=*/2,
                      /*node_timed_rx=*/false);
    h.tracker.last_copies = 1;
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    EXPECT_NE(h.tracker.last_copies, 1)
        << "and the radio must be back on bursts, not merely the belief";
}

TEST(RealLoraClient, TimeSyncIsEncrypted) {
    // The node only trusts an authenticated downlink once it has a session, so
    // a plaintext TimeSync would be both droppable and spoofable — a spoofed
    // clock is the one input that can make a scheduled node sleep through
    // every event, or wake at the wrong time indefinitely.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();
    esphome::ESPTime::set_timezone_offset(7200);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    drive_session(clock, radio, rol);
    const size_t before = radio.transcript().size();
    clock.tick(1000);

    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (!m) continue;
        EXPECT_NE(m->cmd, proto_sim::LoraClientOperationMessage::Cmd::TimeSync)
            << "TimeSync appeared in PLAINTEXT on the wire";
    }

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, NoTimeSyncWhenHubClockInvalid) {
    // Hub booted but Home Assistant time has not arrived yet. Sending epoch 0
    // would be worse than sending nothing: the node would burn awake radio time
    // receiving a frame it must discard. The next login retries the push.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    const size_t before = radio.transcript().size();
    clock.tick(1000);

    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto inner = decrypt_downlink(radio.transcript()[i], base);
        if (!inner) continue;
        EXPECT_NE(inner->cmd, proto_sim::LoraClientOperationMessage::Cmd::TimeSync)
            << "hub sent TimeSync despite having no valid clock";
    }

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// ---------------------------------------------------------------------------
// P2 — wake beacon handling on the hub. The beacon is how the node's clock
// becomes observable without a serial cable, which is what keeps the
// "no sleep cap" decision honest over time.
// ---------------------------------------------------------------------------
namespace {

// Send an ENCRYPTED uplink from the simulated node into the real listener.
void send_encrypted_uplink(LORAClient& listener, uint32_t base_nonce,
                           uint32_t msgid, uint32_t node_addr, uint32_t subnet,
                           proto_sim::LoraClientResponseMessage inner) {
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = subnet;
    inner.header.senderAddress = node_addr;
    inner.header.msgId         = msgid;

    auto plain = proto_sim::serialize_resp_payload(inner);
    auto enc = seal_uplink_like_node(base_nonce, inner.header, plain);

    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
    outer.encrypted.ciphertext = enc.ciphertext;

    auto bytes = proto_sim::serialize_resp(outer);
    listener.set_response(bytes.data(), bytes.size());
}

proto_sim::LoraClientResponseMessage make_beacon(uint64_t node_epoch, bool clock_valid) {
    proto_sim::LoraClientResponseMessage m;
    m.proto = proto_sim::LoraClientResponseMessage::Proto::Beacon;
    m.beacon.reason        = proto_sim::WakeReason::WAKE_TIMER_CHECKIN;
    m.beacon.nodeEpoch     = node_epoch;
    m.beacon.mode          = proto_sim::NodeMode::MODE_INTERACTIVE;
    m.beacon.voltage       = 11.4f;
    m.beacon.position      = 0.5f;
    m.beacon.sessionResume = true;
    m.beacon.clockValid    = clock_valid;
    m.beacon.fwVersion     = 10013;
    return m;
}

struct BeaconRig {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker tracker;
    LORAClient  rol;
    RealTimeClock time;
    uint32_t base{0};

    void start(std::time_t hub_epoch) {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        ensure_psa_ready();
        rol.set_name("rol");
        rol.set_short_address(18);
        rol.set_subnet_address(2);
        rol.set_address(kMacRol2);
        time.set_now(hub_epoch, /*valid=*/true);
        rol.set_time(&time);
        tracker.register_client(&rol);
        base = drive_session(clock, radio, rol);
    }
    ~BeaconRig() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The HA auto-mode switch must not be reverted by a stale beacon.
//
// The beacon carries the mode the node was in when it woke. If our mode change
// has not reached it yet, that is simply out of date — and publishing it
// overwrites the request with the state the user just changed away from.
//
// Observed live: auto mode switched OFF while the node slept; the next beacon
// arrived 2 s BEFORE the push carrying INTERACTIVE went out, and the switch
// snapped back to ON. The change was still delivered and the node did go
// interactive, so the command was not lost — but the UI showed the wrong mode
// until the following beacon, which on the configured 6 h check-in is six
// hours of lying about what the blind is doing.
//
// schedule_pending() is exactly "the node is not on our version yet", which
// separates an undelivered change from a node that legitimately chose a
// different mode (a button press changes no version, so it still wins).
// ---------------------------------------------------------------------------

TEST(RealLoraClient, StaleBeaconDoesNotRevertARequestedModeChange) {
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);
    ASSERT_NE(rig.base, 0u);

    esphome::switch_::Switch sw;
    rig.rol.set_auto_mode_switch(&sw);

    // The user switches auto mode OFF while the node is asleep; HA now shows
    // OFF. This marks the schedule dirty, so the node is behind our version.
    rig.rol.set_auto_mode(false);
    sw.publish_state(false);
    ASSERT_TRUE(rig.rol.schedule_pending())
        << "precondition: the change is undelivered";

    // The node wakes and beacons the mode AND version it had BEFORE our change.
    auto b = make_beacon(kHubEpoch, /*clock_valid=*/true);
    b.beacon.mode         = proto_sim::NodeMode::MODE_AUTO;
    b.beacon.schedVersion = 0;              // never received our push
    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2, b);

    EXPECT_FALSE(sw.state)
        << "a beacon predating our change must not flip the switch back to the "
           "state the user just changed away from";
}

TEST(RealLoraClient, BeaconStillCorrectsTheSwitchOnceTheNodeIsInSync) {
    // The case the correction exists for, which must keep working: the node is
    // on our version and reports a mode we did not ask for — a button press at
    // the blind, or auto mode refused for want of a clock. That is the node's
    // real state and HA must show it.
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);

    esphome::switch_::Switch sw;
    sw.publish_state(true);                 // HA shows AUTO
    rig.rol.set_auto_mode_switch(&sw);

    // "In sync" means the BEACON reports our version — handle_beacon_ takes
    // node_sched_version_ from the beacon, which is the node telling us where
    // it actually is.
    auto b = make_beacon(kHubEpoch, /*clock_valid=*/true);
    b.beacon.mode         = proto_sim::NodeMode::MODE_INTERACTIVE;  // button press
    b.beacon.schedVersion = rig.rol.schedule_version();
    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2, b);

    ASSERT_TRUE(rig.rol.clock_offset_valid_)
        << "precondition: the beacon must actually have been processed";
    ASSERT_FALSE(rig.rol.schedule_pending())
        << "precondition: the node reported our version, so nothing is pending";
    EXPECT_FALSE(sw.state)
        << "an in-sync node reporting INTERACTIVE is telling us its real state; "
           "HA must not keep showing AUTO for a blind that is not in auto mode";
}

TEST(RealLoraClient, AModeChangeBeaconIsNamedNotRenderedAsAQuestionMark) {
    // WAKE_MODE_CHANGED = 5, appended 2026-09-21. The hub's reason table is
    // POSITIONAL — the index is the wire value — and its bound was a hardcoded
    // `<= 4`. A new value that reached the hub without both being extended
    // would log "?" for the one beacon that exists to tell the hub something
    // new, and the operator would see nothing wrong with the node.
    //
    // Pinned here because the table and the enum live in different repos and
    // are kept in step by hand.
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);
    ASSERT_NE(rig.base, 0u);

    auto b = make_beacon(kHubEpoch, /*clock_valid=*/true);
    b.beacon.reason = proto_sim::WakeReason::WAKE_MODE_CHANGED;
    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2, b);

    EXPECT_EQ(rig.rol.last_beacon_reason_, 5u)
        << "the wire value must survive to the hub unchanged";
    // The PhaseReport is what the beacon exists to carry: a mode-change beacon
    // that did not refresh the belief would be an uplink spent for nothing.
    EXPECT_TRUE(rig.rol.clock_offset_valid_)
        << "it is a normal beacon in every other respect";
}

TEST(RealLoraClient, BeaconClockOffsetIsNodeMinusHub) {
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);
    ASSERT_NE(rig.base, 0u);

    // Node runs 7 s AHEAD of the hub.
    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2,
                          make_beacon(kHubEpoch + 7, /*clock_valid=*/true));

    EXPECT_TRUE(rig.rol.clock_offset_valid_);
    EXPECT_EQ(rig.rol.clock_offset_s_, 7)
        << "offset must be node_epoch - hub_epoch; a sign flip would make a "
           "fast node look slow and send drift correction the wrong way";
    EXPECT_EQ(rig.rol.node_fw_version_, 10013u);
    EXPECT_TRUE(rig.rol.node_session_resume_);
}

TEST(RealLoraClient, BeaconClockOffsetIsNegativeWhenNodeLags) {
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);

    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2,
                          make_beacon(kHubEpoch - 12, /*clock_valid=*/true));

    EXPECT_TRUE(rig.rol.clock_offset_valid_);
    EXPECT_EQ(rig.rol.clock_offset_s_, -12);
}

TEST(RealLoraClient, BeaconWithInvalidClockPublishesNoOffset) {
    // I8's case: a node that has never received a TimeSync reports clockValid
    // = false. Publishing an offset computed from epoch 0 would show a ~56-year
    // drift in Home Assistant and make the sensor useless.
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);

    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2,
                          make_beacon(0, /*clock_valid=*/false));

    EXPECT_FALSE(rig.rol.clock_offset_valid_)
        << "a clockless node must not produce a bogus offset reading";
}

// ---------------------------------------------------------------------------
// B-1: the transmit policy belongs to the FRAME, not to the tracker.
//
// setBurstCopies() was a mutable field on LORATracker, set by the caller at
// enqueue and read by sendTask at dequeue. Those are different moments on
// different tasks, so for the whole 300 s of a drift test EVERY frame the hub
// sent inherited copies=1 — including an ordinary blind command a user pressed
// in Home Assistant, delivered at roughly 5.8 % against a node in the normal
// three-window mode. The old code's comment worried only about leaving the
// field set afterwards, which is the smaller half of the bug.
// ---------------------------------------------------------------------------

TEST(TxPolicy, DefaultsToTheFullBurst) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    h.tracker.send(frame, sizeof(frame));

    ASSERT_EQ(h.tracker.sent_copies.size(), 1u);
    EXPECT_EQ(h.tracker.sent_copies[0], 0) << "0 means txSlotsPerRound";
}

TEST(TxPolicy, SingleCopyIsPerFrameAndDoesNotPersist) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    h.tracker.send(frame, sizeof(frame), {/*copies=*/1, /*stride_ms=*/0});
    h.tracker.send(frame, sizeof(frame));

    ASSERT_EQ(h.tracker.sent_copies.size(), 2u);
    EXPECT_EQ(h.tracker.sent_copies[0], 1);
    EXPECT_EQ(h.tracker.sent_copies[1], 0)
        << "a single-copy frame must not change how the NEXT frame is sent";
}

TEST(TxPolicy, OrdinaryCommandKeepsItsBurstDuringADriftTest) {
    // The regression. Start a real drift test on the real LORAListener, then
    // send an ordinary frame while it is active.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    h.rol.start_drift_test(/*duration_s=*/300, /*grid_ms=*/1100);
    ASSERT_TRUE(h.rol.drift_test_active());

    const size_t before = h.tracker.sent_copies.size();
    uint8_t user_command[16] = {0};
    h.tracker.send(user_command, sizeof(user_command));

    ASSERT_GT(h.tracker.sent_copies.size(), before);
    EXPECT_EQ(h.tracker.sent_copies.back(), 0)
        << "a user command sent during a drift test must still be bursted; "
           "one copy is ~5.8 % delivery to a node in three-window mode";

    h.rol.stop_drift_test();
    EXPECT_FALSE(h.rol.drift_test_active());
}

TEST(TxPolicy, StoppingADriftTestBurstsTheOffCommand) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    h.rol.start_drift_test(/*duration_s=*/300, /*grid_ms=*/1100);
    h.tracker.sent_copies.clear();
    h.rol.stop_drift_test();

    // The OFF frame goes out as a normal burst: the node may already have left
    // continuous RX on its own deadline, and a single copy would likely be lost.
    ASSERT_FALSE(h.tracker.sent_copies.empty());
    for (int c : h.tracker.sent_copies)
        EXPECT_EQ(c, 0) << "the OFF command must be bursted";
}

// ---------------------------------------------------------------------------
// M1, hub half — MAC-0 ping and echo (mac-layer.md sections 5 and 6).
//
// Stage 0 of the frame funnel is the hub's alone: only the sender knows what it
// offered. That is why pings_offered lives here and the node never computes its
// own frame-error rate — it cannot know what it did not hear.
// ---------------------------------------------------------------------------

TEST(MacPing, StartsAndStopsCleanly) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    EXPECT_FALSE(h.rol.mac_ping_active());
    h.rol.start_mac_ping(/*duration_s=*/300, /*grid_ms=*/1100);
    EXPECT_TRUE(h.rol.mac_ping_active());
    h.rol.stop_mac_ping();
    EXPECT_FALSE(h.rol.mac_ping_active());
}

TEST(MacPing, StartIsIdempotentAndDoesNotResetStatsMidRun) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    h.rol.start_mac_ping(300, 1100);
    // A second start must not silently restart the accumulator: the run would
    // then only ever reflect its final segment, which is the shape of bug the
    // drift test already had to fix once.
    h.rol.start_mac_ping(300, 1100);
    EXPECT_TRUE(h.rol.mac_ping_active());
    h.rol.stop_mac_ping();
}

TEST(MacPing, IsPlacedOnTheNodesMarkAsASingleCopy) {
    // 2026-09-21. The ping went out through parent_->send() BARE: no
    // earliest_us, so it left whenever the queue drained. Against a Mode B node
    // — one 29.44 ms window per 1500 ms round — a single unplaced copy lands
    // about 2 % of the time, so a run measured the SEND path and reported it as
    // reception loss.
    //
    // The four MacPing tests above are lifecycle-only: none of them looks at
    // the send SHAPE, so nothing here would have caught it, and nothing would
    // catch a revert either. This is that witness.
    //
    // on_mark matters as much as the placement: it is the node's only licence
    // to read the arrival as a phase sample, so a placed ping both measures
    // reception AND feeds promotion.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);   // startGrid() + set_grid_aligned(true)
    ASSERT_TRUE(h.tracker.gridStarted())
        << "precondition: send_aligned_ only places on a running, aligned grid";

    h.rol.start_mac_ping(/*duration_s=*/300, /*grid_ms=*/1100);
    const size_t before = h.tracker.sent_earliest_us.size();

    proto_sim_timer_fire_all();   // the periodic ping timer

    ASSERT_EQ(h.tracker.sent_earliest_us.size(), before + 1)
        << "exactly one frame — if another timer also sent, the assertions "
           "below would be describing the wrong frame";
    EXPECT_EQ(h.tracker.last_copies, 1)
        << "ONE copy: copies is set explicitly, so §4.6's single-shot decision "
           "does not get to turn a ping into a burst";
    EXPECT_GT(h.tracker.last_earliest_us, 0)
        << "PLACED: an earliest_us of 0 is the bare send this replaced";
    EXPECT_TRUE(h.tracker.last_on_mark)
        << "on the node's own mark, and saying so — without this the node "
           "cannot read the arrival as a phase sample";
    EXPECT_EQ(h.rol.mac_stats().pings_offered, 1u)
        << "counted once it actually entered the queue";

    h.rol.stop_mac_ping();
}

TEST(MacPing, WithoutAGridThePingIsUnplacedRatherThanRefused) {
    // The other half of the contract, so the fallback is a decision rather than
    // an accident. send_aligned_ declines to place when the grid is not running
    // or this node is not aligned, and falls through to an ordinary send. That
    // is right for a Mode A node — it sweeps a free-running window and has no
    // mark to aim at — but it silently turns a Mode B run into the 2 % lottery,
    // which is why start_mac_ping warns when it sees this state.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    ASSERT_FALSE(h.tracker.gridStarted()) << "precondition: no grid";

    h.rol.start_mac_ping(/*duration_s=*/300, /*grid_ms=*/1100);
    const size_t before = h.tracker.sent_earliest_us.size();

    proto_sim_timer_fire_all();

    ASSERT_EQ(h.tracker.sent_earliest_us.size(), before + 1);
    EXPECT_EQ(h.tracker.last_copies, 1) << "still one copy, never a burst";
    EXPECT_EQ(h.tracker.last_earliest_us, 0) << "unplaced: nothing to aim at";
    EXPECT_FALSE(h.tracker.last_on_mark)
        << "and it must NOT claim a mark it never occupied — that flag is what "
           "the node trusts to take a phase sample";

    h.rol.stop_mac_ping();
}

TEST(MacPing, EchoIsCountedAndConsumedNotForwarded) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();
    // Was passing on uninitialized-memory luck (registered_ had no default
    // initializer) until the LORAListener layout shifted and it started
    // reading false — see lora_client.h's fix. Set it explicitly, same as
    // the placement tests above.
    h.rol.registered_ = true;

    h.rol.start_mac_ping(300, 1100);
    ASSERT_EQ(h.rol.mac_stats().echoes_rx, 0u);

    // Feed a plaintext MAC echo in, exactly as the node's MAC-0 emits it.
    ::LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = 1;
    hdr.destsubnet    = 2;
    hdr.senderaddress = 2;
    hdr.msgid         = 500;

    ::MacControl echo = MAC_CONTROL__INIT;
    echo.kind = MAC_CONTROL__KIND__MAC_ECHO;
    echo.seq  = 1;

    ::LoraClientResponseMessage resp = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    resp.header     = &hdr;
    resp.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
    resp.maccontrol = &echo;

    std::vector<uint8_t> bytes(lora_client_response_message__get_packed_size(&resp));
    lora_client_response_message__pack(&resp, bytes.data());
    h.rol.set_response(bytes.data(), bytes.size());

    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 1u);
    EXPECT_EQ(h.rol.mac_stats().last_seq_echoed, 1u);
    h.rol.stop_mac_ping();
}

TEST(MacPing, MissingMarksAreCountedAsGapsBySeq) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    h.rol.start_mac_ping(300, 1100);

    auto feed_echo = [&](uint32_t seq, uint32_t msgid) {
        ::LoraHeader hdr = LORA_HEADER__INIT;
        hdr.destaddress = 1; hdr.destsubnet = 2; hdr.senderaddress = 2;
        hdr.msgid = msgid;
        ::MacControl e = MAC_CONTROL__INIT;
        e.kind = MAC_CONTROL__KIND__MAC_ECHO;
        e.seq  = seq;
        ::LoraClientResponseMessage r = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
        r.header = &hdr;
        r.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
        r.maccontrol = &e;
        std::vector<uint8_t> b(lora_client_response_message__get_packed_size(&r));
        lora_client_response_message__pack(&r, b.data());
        h.rol.set_response(b.data(), b.size());
    };

    // Marks 1 and 4 come back; 2 and 3 were lost. A lost frame must leave a
    // HOLE rather than shifting every later sample — which is exactly why seq
    // is carried separately from msgid.
    feed_echo(1, 600);
    feed_echo(4, 601);

    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 2u);
    EXPECT_EQ(h.rol.mac_stats().echo_seq_gaps, 2u);
    EXPECT_EQ(h.rol.mac_stats().last_seq_echoed, 4u);
    h.rol.stop_mac_ping();
}

TEST(MacPing, FirstEchoIsNotAGapEvenIfItsSeqIsHigh) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    h.rol.start_mac_ping(300, 1100);

    ::LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = 1; hdr.destsubnet = 2; hdr.senderaddress = 2; hdr.msgid = 700;
    ::MacControl e = MAC_CONTROL__INIT;
    e.kind = MAC_CONTROL__KIND__MAC_ECHO;
    e.seq  = 9;
    ::LoraClientResponseMessage r = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    r.header = &hdr;
    r.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
    r.maccontrol = &e;
    std::vector<uint8_t> b(lora_client_response_message__get_packed_size(&r));
    lora_client_response_message__pack(&r, b.data());
    h.rol.set_response(b.data(), b.size());

    // Without the have_echo guard this would report eight phantom losses on the
    // very first echo of every run.
    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 1u);
    EXPECT_EQ(h.rol.mac_stats().echo_seq_gaps, 0u);
    h.rol.stop_mac_ping();
}

TEST(MacPing, APingIsNeverAnsweredByTheHub) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();
    h.rol.start_mac_ping(300, 1100);

    ::LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = 1; hdr.destsubnet = 2; hdr.senderaddress = 2; hdr.msgid = 800;
    ::MacControl ping = MAC_CONTROL__INIT;
    ping.kind = MAC_CONTROL__KIND__MAC_PING;   // wrong direction on purpose
    ping.seq = 1;
    ping.wantecho = true;
    ::LoraClientResponseMessage r = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    r.header = &hdr;
    r.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
    r.maccontrol = &ping;
    std::vector<uint8_t> b(lora_client_response_message__get_packed_size(&r));
    lora_client_response_message__pack(&r, b.data());

    const size_t before = h.tracker.sent_copies.size();
    h.rol.set_response(b.data(), b.size());

    // Neither counted as an echo nor answered. If the hub replied to a ping and
    // the node replied to an echo, the two would trade frames forever.
    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 0u);
    EXPECT_EQ(h.tracker.sent_copies.size(), before);
    h.rol.stop_mac_ping();
}

// ---------------------------------------------------------------------------
// B1 — the grid anchor (implementation-plan.md 4.2).
//
// A is set once and never moved. That is what lets a node hold a phase across
// hours, and it is why a hub restart invalidates every node's phase at once.
// ---------------------------------------------------------------------------

TEST(GridAnchor, StartsOnceAndNeverMoves) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};

    h.tracker.startGrid();
    ASSERT_TRUE(h.tracker.gridStarted());
    const int64_t a = h.tracker.gridAnchorUs();

    h.tracker.startGrid();   // a second call must be a no-op
    EXPECT_EQ(h.tracker.gridAnchorUs(), a);
}

TEST(GridAnchor, SlotsAreOnePitchApartWithinARound) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    for (uint8_t k = 0; k + 1 < timedgrid::kSlotCount; ++k) {
        const int64_t t0 = h.tracker.nextT0ForSlotUs(k, a);
        const int64_t t1 = h.tracker.nextT0ForSlotUs(k + 1, a);
        EXPECT_EQ(t1 - t0, (int64_t) timedgrid::kSlotPitchUs) << "slots " << (int) k;
    }
}

TEST(GridAnchor, NextT0IsNeverInThePast) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    // Sweep a whole round in 1 ms steps, for a slot in the middle of the grid.
    for (int64_t off = 0; off < (int64_t) timedgrid::kRoundUs; off += 1000) {
        const int64_t now = a + off;
        const int64_t t0  = h.tracker.nextT0ForSlotUs(7, now);
        EXPECT_GE(t0, now) << "offset " << off;
        EXPECT_LT(t0 - now, (int64_t) timedgrid::kRoundUs) << "offset " << off;
    }
}

TEST(GridAnchor, EveryAnswerIsOnTheGrid) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    for (uint8_t k : {uint8_t{0}, uint8_t{1}, uint8_t{31}})
        for (int64_t off = 0; off < 3LL * timedgrid::kRoundUs; off += 7777) {
            const int64_t t0 = h.tracker.nextT0ForSlotUs(k, a + off);
            const int64_t rel = t0 - a - (int64_t) k * timedgrid::kSlotPitchUs;
            EXPECT_EQ(rel % (int64_t) timedgrid::kRoundUs, 0)
                << "slot " << (int) k << " offset " << off;
        }
}

TEST(GridAnchor, ATimeBeforeTheSlotsFirstT0DoesNotRoundBackwards) {
    // Integer division truncates towards zero, so a negative delta with a
    // (d + round - 1) / round formula rounds the WRONG way and returns an
    // instant in the past. Slot 31's first T0 is 1.45 s after the anchor, so
    // any `now` in that window exercises it.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    const int64_t first_t0 = a + (int64_t) 31 * timedgrid::kSlotPitchUs;
    for (int64_t now = a; now < first_t0; now += 10000) {
        const int64_t t0 = h.tracker.nextT0ForSlotUs(31, now);
        EXPECT_EQ(t0, first_t0) << "now " << now;
        EXPECT_GE(t0, now);
    }
}

TEST(GridAnchor, SlotIndexWrapsRatherThanRunningOffTheGrid) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();
    EXPECT_EQ(h.tracker.nextT0ForSlotUs(timedgrid::kSlotCount, a),
              h.tracker.nextT0ForSlotUs(0, a));
}

TEST(GridAnchor, WithoutAGridTheAnswerIsNow) {
    // A caller that ignores gridStarted() must send immediately, not at some
    // instant derived from a zero anchor.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ASSERT_FALSE(h.tracker.gridStarted());
    EXPECT_EQ(h.tracker.nextT0ForSlotUs(5, 123456), 123456);
}

// ---------------------------------------------------------------------------
// B1 — grid-aligned downlink, default OFF.
//
// Alignment costs up to a full round (1.5 s) of latency on a user command and
// buys nothing until the node opens a single window at that instant (B3). A
// node in Mode A hears three windows every 1.5 s wherever the burst starts, so
// enabling it now would be a pure regression. These tests pin that default as
// hard as they pin the mechanism.
// ---------------------------------------------------------------------------

TEST(GridAligned, DefaultsToOffSoNothingRegresses) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    EXPECT_FALSE(h.rol.grid_aligned());
}

TEST(GridAligned, SlotsAreClaimedInDeclarationOrder) {
    // Same mechanism as login_slot_: claimed in setup(), in YAML order, so it
    // is stable across reboots with no configuration. RealHubHarness does not
    // call setup() (it would restore NVS and schedule logins), so this test
    // drives it explicitly — which is also what makes it a real check of the
    // claim path rather than of a default-initialised member.
    using namespace real_helpers;
    RealHubHarness a{2, kMacRol2};
    RealHubHarness b{3, kMacRol2};
    a.rol.setup();
    b.rol.setup();

    EXPECT_NE(a.rol.grid_slot(), b.rol.grid_slot());
    EXPECT_LT(a.rol.grid_slot(), timedgrid::kSlotCount);
    EXPECT_LT(b.rol.grid_slot(), timedgrid::kSlotCount);
}

TEST(GridAligned, SlotWrapsAtTheGridDepth) {
    // A 33rd client must share a slot rather than address one that does not
    // exist. Asserting the rule directly; the claim itself is above.
    for (uint32_t login = 0; login < 70; ++login)
        EXPECT_LT((uint8_t) (login % timedgrid::kSlotCount), timedgrid::kSlotCount);
    EXPECT_EQ(32u % timedgrid::kSlotCount, 0u);
}

TEST(GridAligned, DelayIsZeroWithoutAGrid) {
    // A caller must never wait on an anchor that was never set.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ASSERT_FALSE(h.tracker.gridStarted());
    EXPECT_EQ(h.tracker.msUntilNextT0(5), 0u);
}

TEST(GridAligned, DelayRoundsUpSoAFrameNeverLandsEarly) {
    // Arriving a millisecond early puts the frame in the previous slot's tail.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();

    // Slot 1's T0 is 46875 us after the anchor. Ask 1 us after the anchor:
    // 46874 us remain, which must round UP to 47 ms, not down to 46.
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;
    EXPECT_EQ(h.tracker.msUntilNextT0(1), 47u);

    // Exactly at a T0 the answer is 0, not a whole round.
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + timedgrid::kSlotPitchUs;
    EXPECT_EQ(h.tracker.msUntilNextT0(1), 0u);
}

TEST(GridAligned, DelayNeverExceedsOneRound) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    for (uint8_t k = 0; k < timedgrid::kSlotCount; ++k)
        for (int64_t off = 0; off < (int64_t) timedgrid::kRoundUs; off += 37000) {
            h.tracker.sim_now_us = h.tracker.gridAnchorUs() + off;
            EXPECT_LE(h.tracker.msUntilNextT0(k), timedgrid::kRoundUs / 1000)
                << "slot " << (int) k << " off " << off;
        }
}

TEST(GridAligned, WithAlignmentOffTheFrameGoesOutImmediately) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1000;   // mid-slot

    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const size_t before = h.tracker.sent_copies.size();
    h.rol.send_aligned_for_test(frame, sizeof(frame));
    EXPECT_EQ(h.tracker.sent_copies.size(), before + 1u)
        << "the default path must not defer anything";
}

TEST(GridAligned, WithAlignmentOnTheFrameIsDeferredToT0) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    // 1 us past the anchor: this client's slot T0 is a whole pitch away unless
    // it happens to own slot 0, so force a slot that is definitely ahead.
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    h.rol.send_aligned_for_test(frame, sizeof(frame));

    // The frame is handed to the transmit queue IMMEDIATELY, carrying the mark
    // it is to be fired at. It used to be held here in an ESPHome timeout and
    // handed over near the right time, which could only ever deliver it to the
    // back of the queue — the queue owns the radio, so only the queue can fire
    // it ON the mark (B5). What "deferred" means, therefore, is the instant
    // that travels with it, not whether send() has been called yet.
    // earliest_us is a FIRE instant; what the node's window is built around is
    // the T0, kPreambleToT0Us later. Asserting the fire instant against
    // nextClearT0ForSlotUs() — the very expression send_aligned_ evaluates —
    // would restate the code back at itself AND hide the missing conversion,
    // which is exactly how a 3136 us placement error survived review.
    const int64_t fire    = h.tracker.last_earliest_us;
    const int64_t seen_t0 = fire + (int64_t) loratiming::kDownlinkPreambleToT0Us;

    // The real contract: that T0 is a mark of THIS node's slot. A mark is a
    // fixed point of nextT0ForSlotUs — asking for the next mark strictly after
    // the instant just before it must return it.
    EXPECT_EQ(h.tracker.nextT0ForSlotUs(h.rol.grid_slot(), seen_t0 - 1), seen_t0)
        << "the frame's T0 must land ON a mark of this node's slot, not "
           "kPreambleToT0Us past one";
    EXPECT_GT(seen_t0, h.tracker.sim_now_us)
        << "and that mark must be in the future, or nothing was placed at all";
}

TEST(GridAligned, ADroppedFrameDoesNotConsumeItsMark) {
    // send() drops silently when the buffer pool is exhausted or the handoff
    // queue is full — it used to return void, so the placement path could not
    // tell. It recorded the mark as spent anyway, which pushed the NEXT command
    // for this node a further round out to make room for a frame that had never
    // been queued: a real command delayed 1.5 s by a phantom.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    uint8_t frame[4] = {1, 2, 3, 4};

    // First frame is dropped by the transmit queue.
    h.tracker.drop_next_sends = 1;
    h.rol.send_aligned_for_test(frame, sizeof(frame));
    const int64_t dropped_at = h.tracker.last_earliest_us;

    // The next real command must get THAT mark, not the one after it. The
    // placement is unchanged; only the bookkeeping was wrong.
    h.rol.send_aligned_for_test(frame, sizeof(frame));
    EXPECT_EQ(h.tracker.last_earliest_us, dropped_at)
        << "a mark spent on a frame that was never queued is a mark lost";
}

TEST(GridAligned, ASecondCommandInTheSameRoundGoesToTheNextMark) {
    // The old deferral was a NAMED ESPHome timeout, so a second command for the
    // same node before the first had fired replaced it: a dropped command,
    // silently, whenever two arrived inside one round. One frame per mark was
    // the right invariant; dropping was never the way to get it.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    uint8_t first[4] = {1, 2, 3, 4};
    h.rol.send_aligned_for_test(first, sizeof(first));
    const int64_t mark1 = h.tracker.last_earliest_us;

    uint8_t second[4] = {5, 6, 7, 8};
    h.rol.send_aligned_for_test(second, sizeof(second));
    const int64_t mark2 = h.tracker.last_earliest_us;

    EXPECT_EQ(h.tracker.sent_copies.size(), (size_t) 2)
        << "both commands must reach the radio; neither may be replaced";
    EXPECT_GT(mark2, mark1) << "the second must be placed at a LATER mark";
    EXPECT_EQ(mark2 - mark1, (int64_t) timedgrid::kRoundUs)
        << "and the next mark for this node is exactly one round on";
}

TEST(GridAligned, TheDeferredFrameSurvivesTheCallersFree) {
    // Every caller frees its packed buffer the moment send_aligned_ returns, so
    // a deferred send that captured the pointer would hand the scheduler freed
    // memory. Write a pattern, free it, then let the timer fire.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    auto *heap = static_cast<uint8_t *>(malloc(16));
    for (int i = 0; i < 16; ++i) heap[i] = static_cast<uint8_t>(0xC0 + i);
    h.rol.send_aligned_for_test(heap, 16);
    memset(heap, 0xEE, 16);   // poison, as a real free+reuse would
    free(heap);

    h.clock.tick(2000);
    SUCCEED() << "no use-after-free; the copy is what was transmitted";
}

// ---------------------------------------------------------------------------
// B4 — pack once, retransmit the stored bytes.
//
// Re-packing on retry was two bugs at once: a fresh msgid made the node execute
// the command a SECOND time (a blind that moves twice), and re-reading
// op_position_ at pack time meant a user moving the blind mid-retry produced
// different plaintext under a msgid-derived AEAD nonce.
// ---------------------------------------------------------------------------

TEST(TrackedOpRetry, ARetransmitReusesTheMsgidAndTheExactBytes) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.5f);
    h.clock.tick(10);
    const auto first = h.radio.hub_to_node_frames();
    ASSERT_FALSE(first.empty()) << "the command must go out at all";
    const auto original = first.back();

    // Let the retry timer fire without any ack.
    h.clock.tick(3000 + 50);   // kOpRetryIntervalMs
    const auto after = h.radio.hub_to_node_frames();
    ASSERT_GT(after.size(), first.size()) << "a retry must be transmitted";
    const auto retry = after.back();

    EXPECT_EQ(retry.bytes, original.bytes)
        << "byte-identical: same msgid, same ciphertext, no GCM nonce reuse "
           "and no second execution at the node";
}

TEST(TrackedOpRetry, ANewCommandGetsANewMsgid) {
    // Pack-once must not freeze the client: a fresh user command is a NEW
    // command and must supersede, with its own msgid. Only the RETRY of an
    // in-flight command reuses bytes.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.25f);
    h.clock.tick(10);
    const auto first = h.radio.hub_to_node_frames().back();

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.90f);
    h.clock.tick(10);
    const auto second = h.radio.hub_to_node_frames().back();

    EXPECT_NE(first.bytes, second.bytes)
        << "a new command is not a retransmission";
}

TEST(TrackedOpRetry, ASecondGestureSupersedesTheFirstOnTheWire) {
    // Section 11b: "one Home Assistant gesture can become two commands 1.5 s
    // apart." The hub tracks exactly ONE command per node — begin_tracked_op_
    // overwrites op_first_msgid_ — so from the instant the second arrives the
    // hub will neither accept the first's ack nor retry it. The first frame was
    // nevertheless still in the transmit queue, placed at an earlier mark, and
    // went out anyway.
    //
    // What the listener can say is what the frame CARRIES. Whether it is then
    // dropped at the front of the queue is the tracker's half, and is tested
    // against the real queue in real_lora_tracker_test.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    h.clock.tick(10);
    const uint32_t gen_first = h.tracker.last_supersede_gen;
    EXPECT_EQ(h.tracker.last_supersede_key, 18u)
        << "the key is the node's address: one node's command must not retire "
           "another's";
    EXPECT_NE(gen_first, 0u) << "0 means 'takes no part', which a command does";

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_CLOSE, 0.0f);
    h.clock.tick(10);
    EXPECT_GT(h.tracker.last_supersede_gen, gen_first)
        << "a new logical command must retire the one still waiting for its mark";
}

TEST(TrackedOpRetry, ARetryDoesNotSupersedeTheFrameItIsARetryOf) {
    // The half that is easy to get wrong. A retransmission is the SAME logical
    // command; giving it a fresh generation would retire the original frame —
    // the bug this fixes, with an extra step, and it would show up only when a
    // retry overtook a placed original.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.5f);
    h.clock.tick(10);
    const uint32_t gen = h.tracker.last_supersede_gen;

    h.clock.tick(3000 + 50);   // kOpRetryIntervalMs, no ack
    EXPECT_EQ(h.tracker.last_supersede_gen, gen)
        << "a retry rides under the generation its frame was built with";
}

TEST(TrackedOpRetry, RoutineDownlinksTakeNoPartInSupersession) {
    // Everything that is not a tracked op carries key 0. A GridSync or a
    // schedule push retired by a cover command would be a frame silently lost
    // because somebody moved a blind.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_login();
    h.clock.tick(10);
    EXPECT_EQ(h.tracker.last_supersede_key, 0u)
        << "a login is not a tracked op and must never retire one";
}

// NOTE on what is NOT tested here. The sharpest form of the hazard —
// op_position_ changing between the pack and the retry WITHOUT a new command —
// is not reachable through the public API in this harness, because
// send_cover_operation() always starts a fresh tracked op. Pack-once removes it
// by construction (the retry never re-reads live state), and that is verified
// above by byte identity rather than by reproducing the mutation.

// ---------------------------------------------------------------------------
// The ModeTest report, as numbers.
//
// The hub recomputes every rate from the node's RAW counters — I1: the node
// never grades itself — and logged them in one line that only a console can
// read. B3's gate is "reception >= Mode A over a week", which is a week of
// Home Assistant history, so the numbers have to exist as numbers. A single
// text_sensor cannot carry them either: HA caps a state at 255 characters and
// the line is up to 512.
// ---------------------------------------------------------------------------

TEST(RealLoraClient, AModeTestReportIsKeptAsNumbersNotJustLogged) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    EXPECT_FALSE(rol.mode_test_summary().valid)
        << "nothing has been measured yet — the entity must read unknown, not 0";

    // A report as the node sends one: raw counters, no conclusions.
    Hist phase = HIST__INIT;
    phase.p50 = 120; phase.p99 = 900; phase.max = 1500; phase.n = 64;
    Hist turn  = HIST__INIT;
    turn.p50 = 4000; turn.p99 = 7000; turn.n = 64;

    ModeTestReport rep = MODE_TEST_REPORT__INIT;
    rep.mode          = 2;            // what the node APPLIED
    rep.armrefusal    = 0;
    rep.elapseds      = 300;
    rep.seqfirst      = 1;
    rep.seqlast       = 100;          // 100 offered over the span it observed
    rep.detected      = 95;
    rep.crcvalid      = 94;
    rep.addressed     = 90;
    rep.windowsarmed  = 100;
    rep.windowshit    = 90;
    rep.phaseerrus    = &phase;
    rep.turnaroundus  = &turn;
    rep.powerprofileproduction = true;
    // Mode B's goal: the run's clock rate. Distinct, non-round values so a
    // field copied into the wrong slot cannot pass by coincidence.
    rep.ppmestimate      = -13;
    rep.ppmsamples       = 187;
    rep.measuredperiodus = 1499981;
    rep.residualppm      = 7;
    rep.residualsamples  = 41;

    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = esphome::lora_tracker::kHubAddress;
    hdr.destsubnet    = 2;
    hdr.senderaddress = 18;
    hdr.msgid         = 1;

    LoraClientResponseMessage msg = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    msg.header         = &hdr;
    msg.proto_case     = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MODETESTREPORT;
    msg.modetestreport = &rep;

    std::vector<uint8_t> frame(lora_client_response_message__get_packed_size(&msg));
    lora_client_response_message__pack(&msg, frame.data());
    rol.set_response(frame.data(), frame.size());

    const auto &s = rol.mode_test_summary();
    ASSERT_TRUE(s.valid) << "a report must reach the summary, not only the log";
    EXPECT_EQ(s.mode, 2u);
    EXPECT_EQ(s.arm_refusal, 0u);
    EXPECT_EQ(s.windows_armed, 100u);
    EXPECT_EQ(s.windows_hit, 90u);
    EXPECT_EQ(s.phase_p99_us, 900);
    EXPECT_EQ(s.turnaround_p99_us, 7000);
    // Recomputed here, not taken from the node: 10 of 100 windows missed.
    EXPECT_EQ(s.wmr_ppm, 100000u);
    // Link loss is CRC-VALID over offered — the air, before any addressing or
    // MAC decision — against the node's own seqFirst..seqLast span, which is
    // the honest denominator: the hub's own count would report the node's late
    // arrival as packet loss. 94 of 100, so 6 %.
    EXPECT_EQ(s.fer_link_ppm, 60000u);
    // The run's clock rate must reach the summary: it is what Mode B is judged
    // on, and until now it travelled on the wire and was read by nothing.
    EXPECT_EQ(s.ppm_estimate, -13);
    EXPECT_EQ(s.ppm_samples, 187u);
    EXPECT_EQ(s.measured_period_us, 1499981);
    // Mode B pass line: the residual after clock discipline, kept apart from
    // the raw rate above.
    EXPECT_EQ(s.residual_ppm, 7);
    EXPECT_EQ(s.residual_samples, 41u);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// ---------------------------------------------------------------------------
// The routine downlinks are PLACED (B1a's last open half)
//
// send_aligned_ was reached only by the tracked-op path and the grid
// publication. TimeSync, ScheduleConfig and BaseNonceExchange went out through
// the bare parent_->send(), so in Mode B they left whenever the queue drained
// — into a window the node had stopped opening. The largest frame on the link
// (a 152 B ScheduleConfig) was also the one least likely to be heard.
//
// Whether each may be sent as ONE copy is a separate question from whether it
// is placed, and it turns on one thing: is the loss visible?
// ---------------------------------------------------------------------------

TEST(PlacedDownlinks, AScheduleConfigIsPlacedAndMayBeASingleShot) {
    // Single-shot ELIGIBLE, and the only one of the three that is: the node
    // ACKS a schedule push, so a missed copy is visible and the retry is
    // already a burst. That is precisely Rule 4's bounded exposure.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.tracker.gridStarted());
    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    const size_t before = h.tracker.sent_earliest_us.size();
    h.rol.send_schedule_config();
    ASSERT_GT(h.tracker.sent_earliest_us.size(), before)
        << "the schedule push must have been sent";

    EXPECT_GT(h.tracker.last_earliest_us, 0)
        << "PLACED: a schedule push sent bare leaves whenever the queue drains, "
           "which in Mode B is not when the node is listening";
    EXPECT_EQ(h.tracker.last_copies, 1)
        << "and it is single-shot eligible, because its loss is visible";
}

TEST(PlacedDownlinks, ATimeSyncIsPlacedButAlwaysABurst) {
    // A TimeSync carries NO ack. Rule 4's "being wrong costs one frame" rests
    // on the loss being visible; for an unacked frame a missed single copy is
    // silent and the node runs on a stale clock until the next push. So the
    // burst is asked for explicitly, even on a node the hub would otherwise
    // trust with one copy.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    const size_t before = h.tracker.sent_earliest_us.size();
    h.rol.send_timesync();
    ASSERT_GT(h.tracker.sent_earliest_us.size(), before);

    EXPECT_GT(h.tracker.last_earliest_us, 0) << "placed";
    EXPECT_TRUE(h.tracker.last_on_mark)
        << "placed on the node's own mark, so it must say so: that flag is the "
           "node's only licence to read its arrival as a phase sample";
    EXPECT_NE(h.tracker.last_copies, 1)
        << "an unacked frame must not be reduced to one copy: nothing would "
           "ever learn it was lost";
}

TEST(PlacedDownlinks, ATimeSyncIsDroppedRatherThanBurstAtASleepingNode) {
    // The burst above is justified by "a TimeSync carries no ack, so a lost
    // single copy is silent". That argument assumes something is LISTENING.
    //
    // A Class A node whose RX1 and RX2 have both closed is asleep until its
    // next wake, and the hub knows it exactly — it placed those windows off
    // the node's own uplink. 17 copies then buy nothing and cost 1408 ms of
    // air that collides with every other node's window.
    //
    // The test above is this one's positive control: a node the hub CANNOT
    // place still bursts. Only the case the hub can place, and places to
    // "nowhere", is dropped.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    // Class A: the node said AUTO in a beacon, and it is on no grid.
    h.rol.node_mode_ = (uint32_t) NODE_MODE__MODE_AUTO;

    constexpr int64_t kT0Uplink = 1'000'000;
    h.rol.last_uplink_t0_us_ = kT0Uplink;
    proto_sim_timer_set_now_us(kT0Uplink + (int64_t) classa::kRx2DelayUs + 1);

    const size_t before = h.tracker.sent_earliest_us.size();
    h.rol.send_timesync();
    EXPECT_EQ(h.tracker.sent_earliest_us.size(), before)
        << "both windows are past: nothing is listening, so nothing goes out";
}

TEST(PlacedDownlinks, ATimeSyncCarriesTheHubsInSlotCountToTheNode) {
    // U-4. NodeState::in_slot_uplinks is §4.6's own promotion criterion and the
    // node cannot measure it: where its uplink landed is produced by its
    // TRANSMIT path — CAD, a burst-end deferral, a random backoff — and the
    // question is where the frame ARRIVED. "A beacon saying I am ready says
    // nothing about where its window actually landed."
    //
    // So the node had it hardcoded to the value that satisfies the criterion,
    // which meant Demotion::NotConfirmed could never fire. The hub has been
    // counting the real thing all along (noteUplinkPlacement_) with no way to
    // tell the node. TimeSync is the carrier because the hub answers every
    // beacon with one.
    //
    // Decoded with the REAL generated stub rather than the mirror, so this
    // asserts the wire bytes and not a second definition of them.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);

    // Three uplinks landing in this node's slot: what the node is not allowed
    // to claim for itself.
    feed_in_slot_uplink(h, 41);
    feed_in_slot_uplink(h, 42);
    feed_in_slot_uplink(h, 43);
    ASSERT_EQ(h.rol.hubBelief().in_slot_acks, 3u)
        << "precondition: the hub has observed three in slot";

    const size_t before = h.radio.hub_to_node_frames().size();
    h.rol.send_timesync();
    ASSERT_GT(h.radio.hub_to_node_frames().size(), before);

    int seen = 0;
    for (const auto &f : h.radio.hub_to_node_frames()) {
        LoraClientOperationMessage *m = lora_client_operation_message__unpack(
            nullptr, f.bytes.size(), f.bytes.data());
        if (m == nullptr) continue;
        if (m->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC &&
            m->timesync != nullptr) {
            EXPECT_EQ(m->timesync->inslotuplinks, 3u)
                << "the hub must tell the node what it has actually observed — "
                   "this is the only route by which the node's own promotion "
                   "criterion can ever be satisfied";
            ++seen;
        }
        lora_client_operation_message__free_unpacked(m, nullptr);
    }
    EXPECT_EQ(seen, 1) << "exactly one TimeSync";
}

TEST(PlacedDownlinks, ARoutineDownlinkDoesNotEraseRuleFoursExposure) {
    // The defect routing these through send_aligned_ would have made routine.
    //
    // send_aligned_ assigned op_sent_single_shot_ — the flag Rule 4 reads to
    // decide whether an unacked command costs this node its confidence — so
    // ANY later frame overwrote the tracked command's shape. A grid publication
    // already did it (it asks for the burst explicitly, so it cleared the
    // flag); a TimeSync or schedule push would have done it on every push.
    // The flag now belongs to the command it describes.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    // A command that really did go out as one copy.
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    h.clock.tick(10);
    ASSERT_EQ(h.tracker.last_copies, 1) << "precondition: it was a single shot";

    // A TimeSync in between — a burst, and nothing to do with that command.
    h.rol.send_timesync();
    h.clock.tick(10);
    ASSERT_NE(h.tracker.last_copies, 1);

    // Now let the command go unacked. Rule 4 must still fire: one frame of
    // exposure is the whole promise single-shot makes, and it is only kept if
    // the hub can still tell the command was a single shot.
    h.clock.tick(3000 + 50);   // kOpRetryIntervalMs
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "an unacked single shot must put this node back on bursts, however "
           "many routine downlinks happened in between";
}

// ---------------------------------------------------------------------------
// U-5: beacon_missed — a txPolicyFor guard that nothing ever set
//
// §11b called closing it "a feature rather than a fix": the hub would have to
// compare a predicted check-in against an observed one. It turned out the hub
// ALREADY HAS the prediction — next_wake_epoch_(), computed from the node's own
// vendored scheduler so the hub does not transmit at a sleeping node. Only the
// comparison was missing, and until it existed single-shot was decided without
// ever asking whether the node is still there.
// ---------------------------------------------------------------------------

namespace {

// A node the hub can predict: interactive, with a known last beacon.
//
// NOTE WHICH KNOB. next_wake_epoch_ uses sleep_duration_ on the interactive
// branch and checkin_interval_ only on the automatic-mode branch — my first
// draft set the latter and the prediction stayed at the 86400 s default, so
// nothing was ever overdue and three tests failed. They are different
// questions: how long the hub told it to sleep, versus how often it wakes
// itself.
void makePredictable(real_helpers::RealHubHarness &h, uint32_t cycle_s,
                     uint32_t beacon_at, uint32_t now) {
    h.rol.registered_ = true;
    h.rol.set_sleep_duration(cycle_s);
    h.rol.set_checkin_interval(cycle_s);
    h.rol.noteBeaconEpochForTest(beacon_at);
    // The hub put it to sleep right after that beacon: a COMMANDED sleep is what
    // makes the prediction (and so "overdue") meaningful for an interactive node.
    h.rol.last_sleep_epoch_ = beacon_at;
    h.time.set_now(now, /*valid=*/true);
}

}  // namespace

TEST(BeaconMissed, ANodeInsideItsCheckinIntervalIsNotOverdue) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    makePredictable(h, /*cycle_s=*/600, /*beacon_at=*/1000, /*now=*/1300);
    EXPECT_FALSE(h.rol.node_overdue()) << "it is not even due yet";
    EXPECT_FALSE(h.rol.hubBelief().beacon_missed);
}

TEST(BeaconMissed, TheGraceCoversTheNodesWholeBeaconLadder) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    // Due at 1600. The node beacons within seconds of waking and re-beacons
    // twice before its resume fallback fires; kQuietWindowMinMs (17 s) is sized
    // to cover that whole sequence, so a verdict inside the grace would call a
    // node missing while it is still working through its own retries.
    makePredictable(h, /*cycle_s=*/600, /*beacon_at=*/1000, /*now=*/1600 + 30);
    EXPECT_FALSE(h.rol.node_overdue()) << "inside the grace";

    h.time.set_now(1600 + LORAClient::kBeaconOverdueGraceS, /*valid=*/true);
    EXPECT_TRUE(h.rol.node_overdue()) << "past it, and nothing heard since";
}

TEST(BeaconMissed, AMissedCheckinDeniesSingleShotWithItsOwnReason) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    give_phase_report(h);
    makePredictable(h, /*cycle_s=*/600, /*beacon_at=*/1000, /*now=*/1000);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot)
        << "precondition: everything else says this node may take one copy";

    // Now it fails to appear.
    h.time.set_now(1600 + LORAClient::kBeaconOverdueGraceS + 1, /*valid=*/true);

    EXPECT_TRUE(h.rol.hubBelief().beacon_missed);
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "a node that missed the check-in it was predicted to make is not a "
           "node to spend the one copy on";
    EXPECT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::BeaconMissed)
        << "and the published reason must be this one, not a later rung that "
           "happens to also fail";
}

TEST(BeaconMissed, AnAwakeNodeWithAnImminentEventIsNotOverdue) {
    // THE FALSE POSITIVE THAT MADE THE SECOND CLAUSE NECESSARY.
    //
    // In automatic mode next_wake_epoch_ takes the EARLIER of the next
    // scheduled event (minus the beacon lead) and the periodic check-in, and
    // the scheduled-event branch clamps to `now` when the lead exceeds the
    // remaining time. So the prediction can sit at `now` indefinitely for a
    // node with an imminent event — and "the predicted wake has passed by more
    // than the grace" alone would then call a node overdue while it is awake
    // and beaconing normally.
    //
    // The second clause — nothing heard SINCE the predicted wake — is what
    // makes the verdict about the node rather than about the prediction.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    makePredictable(h, /*cycle_s=*/600, /*beacon_at=*/1000, /*now=*/1000);

    // Heard from AFTER the predicted wake instant.
    h.rol.noteBeaconEpochForTest(5000);
    h.time.set_now(5000 + LORAClient::kBeaconOverdueGraceS + 1, /*valid=*/true);
    EXPECT_FALSE(h.rol.node_overdue())
        << "the last beacon is more recent than the predicted wake, so this "
           "node has appeared — whatever the prediction says";
}

TEST(BeaconMissed, ANodeTheHubCannotPredictIsNeverMissed) {
    // FAILS OPEN, deliberately. A prediction the hub cannot make must not deny
    // single-shot permanently: that is exactly how the old in-slot criterion
    // became unsatisfiable, and a guard that can never be cleared is worse
    // than one that never fires.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.time.set_now(900000, /*valid=*/true);
    // nothing has ever been heard from this node
    EXPECT_FALSE(h.rol.node_overdue());

    // And with no hub clock at all, which is the other way the comparison
    // cannot be made.
    makePredictable(h, /*cycle_s=*/600, 1000, 900000);
    h.time.set_now(900000, /*valid=*/false);
    EXPECT_FALSE(h.rol.node_overdue());
}

TEST(BeaconMissed, ABeaconClearsItBecauseThePredictionMovesWithIt) {
    // No explicit reset anywhere: next_wake_epoch_ is computed from the last
    // thing the hub heard, so every beacon pushes the prediction forward and
    // the verdict follows. Worth pinning — a stored flag would have needed a
    // clearing path, and a missed clearing path is how rebooted_since_confirm
    // spent a release stuck true.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    makePredictable(h, /*cycle_s=*/600, /*beacon_at=*/1000,
                    /*now=*/1600 + LORAClient::kBeaconOverdueGraceS + 1);
    ASSERT_TRUE(h.rol.node_overdue());

    h.rol.noteBeaconEpochForTest(static_cast<uint32_t>(1600 + LORAClient::kBeaconOverdueGraceS + 1));
    EXPECT_FALSE(h.rol.node_overdue())
        << "it turned up; the next verdict is due one check-in interval later";
}

TEST(GridAligned, AModeBTestMarkLandsOnTheNodesGridMark) {
    // D4. MEASURED 2026-09-13: Mode B test marks left whenever the hub's
    // esp_timer fired, so they sat at a fixed, arbitrary phase against the
    // grid — all 14 phase samples at -651 ms, ~46 guard bands out.
    // phaseTrustworthy() could never pass, timed RX never armed, and HW-8 had
    // no one-shot samples. The test's own marks made Mode B unreachable.
    //
    // Same contract as SendAlignedHandsTheFrameToTheQueueWithItsMark: the T0
    // the node sees must be a mark of THIS node's slot, and in the future.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;
    proto_sim_timer_set_now_us(h.tracker.sim_now_us);

    h.rol.start_mode_test(/*duration_s=*/300, /*grid_ms=*/1500, /*mode=*/2,
                          /*copies=*/1, /*keep_power_profile=*/true,
                          /*enable_counter=*/false, /*enable_crypto=*/false,
                          /*mac_echo=*/true, /*arm_offset_us=*/0);
    h.rol.mode_test_tick_for_test();

    const int64_t fire    = h.tracker.last_earliest_us;
    ASSERT_GT(fire, 0) << "a Mode B mark must be PLACED, not sent whenever the timer fired";
    const int64_t seen_t0 = fire + (int64_t) loratiming::kDownlinkPreambleToT0Us;
    EXPECT_EQ(h.tracker.nextT0ForSlotUs(h.rol.grid_slot(), seen_t0 - 1), seen_t0)
        << "the mark's T0 must land ON a mark of this node's slot";
    EXPECT_GT(seen_t0, h.tracker.sim_now_us) << "and in the future";

    // A second tick must not aim at the same mark.
    h.rol.mode_test_tick_for_test();
    EXPECT_GT(h.tracker.last_earliest_us + (int64_t) loratiming::kDownlinkPreambleToT0Us, seen_t0)
        << "one mark per grid mark: a jittered tick must move on, not double up";
    h.rol.stop_mode_test();
}

TEST(GridAligned, AModeATestMarkStaysUnplaced) {
    // The other half of D4, and deliberate: Mode A's swept 1093 ms period is
    // what lets a free-running window catch it at all. Placing it on the grid
    // would re-create the phase lock the period was chosen to break.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;
    proto_sim_timer_set_now_us(h.tracker.sim_now_us);

    h.rol.start_mode_test(300, 1093, /*mode=*/1, 1, true, false, false, true, 0);
    h.rol.mode_test_tick_for_test();
    EXPECT_EQ(h.tracker.last_earliest_us, 0)
        << "a Mode A mark must go out unplaced";
    h.rol.stop_mode_test();
}

// ---------------------------------------------------------------------------
// (a') — hearing the node beats the sleep model
// ---------------------------------------------------------------------------
TEST(RealLoraClient, AHeardNodeIsAwakeForItsListeningWindowNotForTheModel) {
    // MEASURED 2026-09-13: "Login retry deferred 21250 s — node asleep until
    // then", logged moments after that node's REGISTER. The model was purely
    // last-recorded-sleep + sleep_duration, so one lost LoginMsg after any
    // recorded sleep would have stalled the session for up to six hours.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    constexpr uint32_t kNow = 1'789'000'000u;
    h.time.set_now(kNow, /*valid=*/true);
    h.rol.last_sleep_epoch_ = kNow - 100;          // told to sleep 100 s ago
    h.rol.node_mode_        = 0;                   // interactive
    ASSERT_FALSE(h.rol.timed_mode_enabled()) << "precondition: no grid";

    ASSERT_GT(h.rol.ms_until_node_awake_for_test(), 0u)
        << "precondition: 100 s into a 6 h sleep the model says asleep";

    auto reg = serialize_register(kMacRol2, /*needs_config=*/false);
    h.rol.set_response(reg.data(), reg.size());

    EXPECT_EQ(h.rol.ms_until_node_awake_for_test(), 0u)
        << "a REGISTER from this node's MAC proves it is awake NOW; retries must "
           "not be stretched to a modelled wake six hours away";

    // Still inside the interactive window: still awake.
    h.time.set_now(kNow + 1799, true);
    EXPECT_EQ(h.rol.ms_until_node_awake_for_test(), 0u)
        << "an interactive node keeps listening for its interactive timeout";

    // Past it: the model governs again.
    h.time.set_now(kNow + 1801, true);
    EXPECT_GT(h.rol.ms_until_node_awake_for_test(), 0u)
        << "once the listening window is over, hearing it no longer says anything";
}

TEST(RealLoraClient, AHeardClassANodeIsAwakeOnlyThroughItsReceiveWindows) {
    // Mode C listens only in RX1/RX2 after its own uplink, then sleeps. Treating
    // a heard Class A node as awake for the interactive half hour would retry
    // into a sleeping node — the airtime the sleep gate exists to save.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    constexpr uint32_t kNow = 1'789'000'000u;
    h.time.set_now(kNow, /*valid=*/true);
    h.rol.last_sleep_epoch_   = kNow - 100;
    h.rol.node_mode_          = (uint32_t) NODE_MODE__MODE_AUTO;
    ASSERT_FALSE(h.rol.timed_mode_enabled()) << "precondition: no grid, so Class A";

    auto reg = serialize_register(kMacRol2, /*needs_config=*/false);
    h.rol.set_response(reg.data(), reg.size());

    h.time.set_now(kNow + 2, true);
    EXPECT_EQ(h.rol.ms_until_node_awake_for_test(), 0u)
        << "inside RX2 the node is still listening";
    h.time.set_now(kNow + 4, true);
    EXPECT_GT(h.rol.ms_until_node_awake_for_test(), 0u)
        << "after RX2 a Class A node is asleep again, however recently it spoke";
}

// ---------------------------------------------------------------------------
// Ack timers are counted from the AIR, not from the queue.
//
// The hub airs one 17-copy burst plus a response window at a time, ~1.85 s. Four
// timers used to start at send(): login retry, schedule retry, GridSync
// re-publish and the tracked-op retry. A frame queued behind four others had not
// left the radio when its 5 s timer expired, so the retransmit joined the same
// queue — one more burst of airtime spent on a frame already in it. Measured
// 2026-09-26 with two nodes after a hub restart: 16 bursts back to back for 30 s,
// every ScheduleConfig "not acknowledged" three times while the node acked each.
//
// Each test stands a queue in front of the frame (tracker.tx_drain_us) and
// asserts the retry does NOT fire at the base delay and DOES fire once the
// backlog has drained, with an idle-queue control proving the base delay itself
// is unchanged.
// ---------------------------------------------------------------------------
namespace {
constexpr int64_t kBacklogUs = 9'000'000;   // ~5 bursts queued ahead
size_t frames_on_air(const real_helpers::RealHubHarness& h) {
    return h.radio.hub_to_node_frames().size();
}
}  // namespace

TEST(AckTimers, AScheduleRetryWaitsOutTheTransmitBacklog) {
    using namespace real_helpers;
    for (const int64_t backlog_us : {int64_t{0}, kBacklogUs}) {
        RealHubHarness h{18, kMacRol2};
        h.rol.registered_ = true;
        h.tracker.tx_drain_us = backlog_us;

        h.rol.send_schedule_config();
        const size_t sent = frames_on_air(h);
        ASSERT_GT(sent, 0u) << "the schedule push must have gone out";

        h.clock.tick(esphome::lora_tracker::LORAListener::kSchedRetryMs + 100);
        if (backlog_us == 0) {
            EXPECT_GT(frames_on_air(h), sent)
                << "idle queue: the retry is due at the base delay, as before";
        } else {
            EXPECT_EQ(frames_on_air(h), sent)
                << "the frame is still queued behind ~9 s of bursts; retrying now "
                   "adds a duplicate to the queue it is already in";
            h.clock.tick(backlog_us / 1000);
            EXPECT_GT(frames_on_air(h), sent)
                << "once the backlog has drained the retry must still happen";
        }
    }
}

TEST(AckTimers, ATrackedOpRetryWaitsOutTheTransmitBacklog) {
    using namespace real_helpers;
    for (const int64_t backlog_us : {int64_t{0}, kBacklogUs}) {
        RealHubHarness h{18, kMacRol2};
        ensure_psa_ready();
        h.rol.registered_ = true;
        h.tracker.tx_drain_us = backlog_us;

        h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.5f);
        h.clock.tick(10);
        const size_t sent = frames_on_air(h);
        ASSERT_GT(sent, 0u);

        h.clock.tick(3000 + 50);   // kOpRetryIntervalMs
        if (backlog_us == 0) {
            EXPECT_GT(frames_on_air(h), sent) << "idle queue: retry at the base delay";
        } else {
            EXPECT_EQ(frames_on_air(h), sent) << "still queued: no duplicate";
            h.clock.tick(backlog_us / 1000);
            EXPECT_GT(frames_on_air(h), sent) << "and it retries after the drain";
        }
    }
}

TEST(AckTimers, AGridSyncRepublishWaitsOutTheTransmitBacklog) {
    using namespace real_helpers;
    for (const int64_t backlog_us : {int64_t{0}, kBacklogUs}) {
        RealHubHarness h{18, kMacRol2};
        h.rol.registered_ = true;
        h.tracker.tx_drain_us = backlog_us;

        h.rol.enable_timed_mode(true);
        ASSERT_TRUE(h.tracker.gridStarted());
        const size_t sent = frames_on_air(h);
        ASSERT_GT(sent, 0u) << "enabling timed mode publishes a GridSync";

        h.clock.tick(esphome::lora_tracker::LORAListener::kGridSyncRetryMs + 100);
        if (backlog_us == 0) {
            EXPECT_GT(frames_on_air(h), sent) << "idle queue: republish at the base delay";
        } else {
            EXPECT_EQ(frames_on_air(h), sent) << "still queued: no duplicate GridSync";
            h.clock.tick(backlog_us / 1000);
            EXPECT_GT(frames_on_air(h), sent) << "and it republishes after the drain";
        }
    }
}

TEST(AckTimers, ALoginRetryWaitsOutTheTransmitBacklog) {
    using namespace real_helpers;
    for (const int64_t backlog_us : {int64_t{0}, kBacklogUs}) {
        RealHubHarness h{18, kMacRol2};
        h.tracker.tx_drain_us = backlog_us;

        auto reg = serialize_register(kMacRol2);
        h.rol.set_response(reg.data(), reg.size());
        h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 100);
        const size_t sent = frames_on_air(h);
        ASSERT_GT(sent, 0u) << "the login challenge must have fired";

        h.clock.tick(esphome::lora_tracker::LORAListener::kLoginRetryBaseMs + 100);
        if (backlog_us == 0) {
            EXPECT_GT(frames_on_air(h), sent) << "idle queue: first retry at the base delay";
        } else {
            EXPECT_EQ(frames_on_air(h), sent) << "the login is still queued: no retry yet";
            h.clock.tick(backlog_us / 1000);
            EXPECT_GT(frames_on_air(h), sent) << "and it retries after the drain";
        }
    }
}

// ---------------------------------------------------------------------------
// Two nodes, one downlink queue: one node's onboarding at a time.
//
// Measured 2026-09-26 after a hub restart with nodes 17 and 18: the second
// login went out 3 s after the first and the two handshakes interleaved for
// 30 s — 16 bursts back to back, every ScheduleConfig timed out while the node
// acked it, and a CoverConfig went out in the clear onto a confirmed session.
// The startup stagger (slot x 3 s) separated only the FIRST login of each node;
// a whole onboarding is ~8 frames and ~15 s of airtime.
// ---------------------------------------------------------------------------
namespace {

constexpr uint64_t kMacRol1 = 0xA4B75FFE8CE0ULL;

struct TwoNodeHub {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker tracker;
    LORAClient  rol_1;
    LORAClient  rol_2;
    RealTimeClock time;

    // `answer_1` / `answer_2`: does that node respond to its login with an
    // encrypted ClientAvailable, as the real node does?
    TwoNodeHub(bool answer_1, bool answer_2) {
        esphome::lora_tracker::LORAListener::reset_onboarding_gate_for_test();
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        ensure_psa_ready();

        rol_1.set_name("rol_1"); rol_1.set_short_address(17); rol_1.set_address(kMacRol1);
        rol_2.set_name("rol_2"); rol_2.set_short_address(18); rol_2.set_address(kMacRol2);
        for (LORAClient* r : {&rol_1, &rol_2}) {
            r->set_subnet_address(2);
            r->set_sleep_duration(21600);
            r->set_time(&time);
            tracker.register_client(r);
        }
        time.set_now(1787000000, /*valid=*/true);
        if (answer_1) attach_encrypted_login_ack(radio, rol_1, 17, 2);
        if (answer_2) attach_encrypted_login_ack(radio, rol_2, 18, 2);
    }
    ~TwoNodeHub() {
        esphome::lora_tracker::LORAListener::reset_onboarding_gate_for_test();
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }

    // Both nodes announce themselves to the hub in the same instant, as they do
    // after a hub restart.
    void bothRegister() {
        auto r1 = real_helpers::serialize_register(kMacRol1, /*needs_config=*/false);
        auto r2 = real_helpers::serialize_register(kMacRol2, /*needs_config=*/false);
        rol_1.set_response(r1.data(), r1.size());
        rol_2.set_response(r2.data(), r2.size());
    }

    int loginsTo(uint32_t addr) const {
        int n = 0;
        for (const auto& f : radio.hub_to_node_frames()) {
            auto m = proto_sim::as_op(f);
            if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login &&
                m->header.destAddress == addr) ++n;
        }
        return n;
    }
};

}  // namespace

TEST(OnboardingGateHub, TheSecondNodeDoesNotStartItsLoginWhileTheFirstIsBeingBroughtUp) {
    TwoNodeHub h(/*answer_1=*/true, /*answer_2=*/true);
    h.bothRegister();

    // Just past the REGISTER->login delay: the first node's login is out.
    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    EXPECT_EQ(h.loginsTo(17), 1) << "the node that registered first goes first";
    EXPECT_EQ(h.loginsTo(18), 0)
        << "node 18's login must WAIT: started now it queues behind node 17's "
           "config, time and schedule pushes and every ack timer in both runs "
           "on frames that have not left the radio";
    EXPECT_TRUE(h.rol_1.onboarding_held_for_test());
    EXPECT_FALSE(h.rol_2.onboarding_held_for_test());

    // It does start once node 17 is settled, and not before.
    bool second_started = false;
    bool first_was_settled_by_then = false;
    for (int i = 0; i < 400 && !second_started; ++i) {
        h.clock.tick(100);
        if (h.loginsTo(18) > 0) {
            second_started = true;
            first_was_settled_by_then = h.rol_1.session_confirmed_ &&
                                        !h.rol_1.onboarding_held_for_test();
        }
    }
    ASSERT_TRUE(second_started) << "the second node must not be starved";
    EXPECT_TRUE(first_was_settled_by_then)
        << "node 17 had to have confirmed its session and released the gate";
    EXPECT_TRUE(h.rol_2.onboarding_held_for_test() || h.rol_2.session_confirmed_);

    // ...and both end up with a confirmed session.
    h.clock.tick(30000);
    EXPECT_TRUE(h.rol_1.session_confirmed_);
    EXPECT_TRUE(h.rol_2.session_confirmed_);
    EXPECT_FALSE(h.rol_1.onboarding_held_for_test());
    EXPECT_FALSE(h.rol_2.onboarding_held_for_test());
}

TEST(OnboardingGateHub, ANodeThatNeverAnswersDoesNotHoldTheOtherNodeHostage) {
    // Node 17 is unplugged. It registered (or the hub restored its state), the
    // hub logs in to it, and nothing comes back. Node 18 must not wait for a
    // handshake that is never going to finish.
    TwoNodeHub h(/*answer_1=*/false, /*answer_2=*/true);
    h.bothRegister();

    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    ASSERT_EQ(h.loginsTo(17), 1);
    ASSERT_EQ(h.loginsTo(18), 0);

    // Not before the node has been given its chance to answer...
    h.clock.tick(onboarding::kSilentAfterMs / 2);
    EXPECT_EQ(h.loginsTo(18), 0) << "silence is judged after a fair wait, not at once";

    // ...but well inside the hold cap.
    h.clock.tick(onboarding::kSilentAfterMs + 4000);
    EXPECT_GE(h.loginsTo(18), 1)
        << "node 17 never answered, so node 18 must have been let through";
    h.clock.tick(30000);
    EXPECT_TRUE(h.rol_2.session_confirmed_);
    EXPECT_FALSE(h.rol_1.session_confirmed_);
}

TEST(OnboardingGateHub, ALoneNodeIsNotDelayedByTheGate) {
    // The gate must cost a single node nothing: same login, same moment.
    TwoNodeHub h(/*answer_1=*/false, /*answer_2=*/true);
    auto r2 = real_helpers::serialize_register(kMacRol2, /*needs_config=*/false);
    h.rol_2.set_response(r2.data(), r2.size());
    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    EXPECT_EQ(h.loginsTo(18), 1) << "no other node is onboarding: no wait";
    EXPECT_TRUE(h.rol_2.session_confirmed_);
}

namespace {
// login_retry_step_ is what the retry timer runs. A probe adds no state; it
// only lets a test run the step at a moment of its choosing.
struct RetryProbe : LORAClient {
    using esphome::lora_tracker::LORAListener::login_retry_step_;
};
}  // namespace

TEST(OnboardingGateHub, ALoginRetryIsNotSentIntoAnotherNodesOnboarding) {
    // A retry is a login challenge like any other. Node 17 is mid-onboarding and
    // holds the gate; node 18's backoff timer fires. Sending it now puts a login
    // into the very queue the gate protects.
    TwoNodeHub h(/*answer_1=*/true, /*answer_2=*/false);
    auto r1 = real_helpers::serialize_register(kMacRol1, /*needs_config=*/false);
    h.rol_1.set_response(r1.data(), r1.size());
    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    ASSERT_TRUE(h.rol_1.onboarding_held_for_test()) << "precondition: node 17 holds the gate";

    h.rol_2.registered_ = true;
    const int before = h.loginsTo(18);
    static_cast<RetryProbe&>(h.rol_2).login_retry_step_();
    EXPECT_EQ(h.loginsTo(18), before)
        << "no login to 18 while 17 is being brought up";
    EXPECT_EQ(h.rol_2.login_retry_count_, 0u)
        << "and the retry that did not happen must not be counted against 18's ladder";

    // Once 17 is done the same step goes through.
    h.clock.tick(60000);
    ASSERT_FALSE(h.rol_1.onboarding_held_for_test());
    h.rol_2.login_acked_ = false;
    static_cast<RetryProbe&>(h.rol_2).login_retry_step_();
    EXPECT_GT(h.loginsTo(18), before) << "a free gate lets the retry through";
}

TEST(OnboardingGateHub, ANodeThatHasBeenHeardKeepsTheGateUntilTheCapNotTheSilenceLimit) {
    // Node 17 answered its REGISTER but its login is not being acknowledged (a
    // slow link, not an absent node). It is worth waiting for longer than a node
    // that never spoke — but not for ever.
    TwoNodeHub h(/*answer_1=*/false, /*answer_2=*/true);
    h.bothRegister();
    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    ASSERT_EQ(h.loginsTo(17), 1);

    // Any frame from node 17 after its login went out. A second REGISTER is one
    // the hub's own handler already treats as proof the node is awake.
    h.clock.tick(2000);
    auto r1 = real_helpers::serialize_register(kMacRol1, /*needs_config=*/false);
    h.rol_1.set_response(r1.data(), r1.size());

    h.clock.tick(onboarding::kSilentAfterMs + 3000);
    EXPECT_EQ(h.loginsTo(18), 0)
        << "node 17 has been heard since its login; past the silence limit it "
           "still holds the gate";
    h.clock.tick(onboarding::kMaxHoldMs);
    EXPECT_GE(h.loginsTo(18), 1) << "but the cap releases it";
}

TEST(OnboardingGateHub, TheGateIsNotReleasedWhileTheSchedulePushIsUnacknowledged) {
    // The schedule push is part of bringing a node up: it is one of the frames
    // in the queue, and its retries are more of them.
    TwoNodeHub h(/*answer_1=*/true, /*answer_2=*/true);
    h.bothRegister();
    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);

    // Past confirmation and the settle delay, but the node has not acked the
    // ScheduleConfig it was sent.
    h.clock.tick(onboarding::kSettleAfterConfirmMs + 2500);
    ASSERT_TRUE(h.rol_1.session_confirmed_);
    ASSERT_NE(h.rol_1.sched_push_msgid_, 0u) << "precondition: a ScheduleConfig is outstanding";
    EXPECT_TRUE(h.rol_1.onboarding_held_for_test())
        << "still bringing node 17 up: its schedule has not landed";
    EXPECT_EQ(h.loginsTo(18), 0);

    // The node acknowledges it: the gate frees within a poll or two.
    static_cast<AckProbe&>(h.rol_1).handle_command_ack_(h.rol_1.sched_push_msgid_);
    h.clock.tick(2 * onboarding::kPollMs + 100);
    EXPECT_FALSE(h.rol_1.onboarding_held_for_test());
}

TEST(OnboardingGateHub, TheGateIsNotReleasedWhileTheNodesOwnFramesAreStillQueued) {
    // Confirmed, settled by the clock, schedule acked - but the hub's queue still
    // holds this node's pushes. Freeing the gate now lets the next node's login
    // land behind them, which is exactly the pile-up.
    TwoNodeHub h(/*answer_1=*/true, /*answer_2=*/true);
    h.bothRegister();
    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    h.clock.tick(onboarding::kSettleAfterConfirmMs + 2500);
    ASSERT_TRUE(h.rol_1.session_confirmed_);
    ASSERT_NE(h.rol_1.sched_push_msgid_, 0u);

    h.tracker.tx_drain_us = 20'000'000;
    static_cast<AckProbe&>(h.rol_1).handle_command_ack_(h.rol_1.sched_push_msgid_);
    h.clock.tick(10000);
    EXPECT_TRUE(h.rol_1.onboarding_held_for_test())
        << "20 s of its own frames are still queued";
    EXPECT_EQ(h.loginsTo(18), 0);

    h.tracker.tx_drain_us = 0;
    h.clock.tick(2 * onboarding::kPollMs + 100);
    EXPECT_FALSE(h.rol_1.onboarding_held_for_test()) << "and it frees once they have gone";
}

// ---------------------------------------------------------------------------
// While the session is being rebuilt, housekeeping pushes wait.
//
// Measured 2026-09-26, node 2 reset with the hub running: the node's beacon made
// the hub queue a TimeSync and a ScheduleConfig, its REGISTER arrived 0.2 s
// later, and the hub's login started before either had been sent. They went
// out anyway - the TimeSync under the old session (the rebooted node could not
// decrypt it) and the ScheduleConfig in the CLEAR, because send_login() had
// already un-confirmed the session (the node refused it: "Rejecting PLAINTEXT
// command (cmd_case=17) ... this node holds a session"). Three wasted bursts,
// and the login itself sat 3 s behind them. confirm_session_ pushes all three
// again once the node has proved the key, so nothing is lost by waiting.
// ---------------------------------------------------------------------------
TEST(SessionRebuild, HousekeepingPushesWaitForTheSessionInsteadOfGoingOutInTheClear) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();

    uint32_t base = 0;
    h.radio.add_sink([&base](const proto_sim::AirFrame& f) {
        if (f.dir != proto_sim::AirFrame::Dir::HubToNode) return;
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            base = m->login.nonce;
    });
    attach_encrypted_login_ack(h.radio, h.rol, /*node_addr=*/18, /*subnet=*/2);

    // The node has rebooted: its REGISTER starts the hub's login, 4 s from now.
    auto reg = serialize_register(kMacRol2, /*needs_config=*/false);
    h.rol.set_response(reg.data(), reg.size());
    const size_t baseline = h.radio.hub_to_node_frames().size();

    // The pushes a beacon had queued, firing inside that window.
    h.rol.send_schedule_config();
    h.rol.send_timesync();
    h.rol.enable_timed_mode(true);
    EXPECT_EQ(h.radio.hub_to_node_frames().size(), baseline)
        << "nothing may leave between the REGISTER and a confirmed session: the "
           "node just restarted, so an old-session frame cannot be decrypted and "
           "a plaintext one is refused. Each is a burst of airtime spent to be dropped";

    // Bring the session up. The pushes now go, encrypted.
    h.clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    ASSERT_TRUE(h.rol.session_confirmed_);
    ASSERT_NE(base, 0u);
    h.clock.tick(4000);

    int plain_schedule = 0, plain_timesync = 0, enc_schedule = 0, enc_timesync = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto plain = proto_sim::as_op(f);
        if (plain && plain->cmd == proto_sim::LoraClientOperationMessage::Cmd::Schedule) ++plain_schedule;
        if (plain && plain->cmd == proto_sim::LoraClientOperationMessage::Cmd::TimeSync) ++plain_timesync;
        auto inner = decrypt_downlink(f, base);
        if (inner && inner->cmd == proto_sim::LoraClientOperationMessage::Cmd::Schedule) ++enc_schedule;
        if (inner && inner->cmd == proto_sim::LoraClientOperationMessage::Cmd::TimeSync) ++enc_timesync;
    }
    EXPECT_EQ(plain_schedule, 0) << "a ScheduleConfig in the clear is refused by a node that holds a session";
    EXPECT_EQ(plain_timesync, 0) << "and so is a TimeSync";
    EXPECT_GE(enc_schedule, 1) << "the schedule must still be delivered, once the session exists";
    EXPECT_GE(enc_timesync, 1) << "and the time";
}

TEST(SessionRebuild, ALoginTheHubStartsItselfHoldsThePushesBackToo) {
    // No REGISTER here: a hub restart makes the hub log in to every node on its
    // own. send_login() un-confirms the session, so a push sent now would be in
    // the clear. The flag has to be raised by the login itself, not only by a
    // REGISTER, or this path is unguarded.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_login();
    const size_t baseline = h.radio.hub_to_node_frames().size();
    ASSERT_GT(baseline, 0u) << "the login itself must have gone out";

    h.rol.send_schedule_config();
    h.rol.send_timesync();
    h.rol.enable_timed_mode(true);
    EXPECT_EQ(h.radio.hub_to_node_frames().size(), baseline)
        << "pushes wait for the node to prove the new key";
}

TEST(SessionRebuild, WithdrawingTheGridIsNotDeferredBecauseItIsTheSafeDirection) {
    // Only a PUBLISH needs the session (it carries the fleet key). Taking a node
    // off the grid is the safe direction, needs no key, and must not be held
    // behind a login that may never complete.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);

    auto reg = serialize_register(kMacRol2, /*needs_config=*/false);
    h.rol.set_response(reg.data(), reg.size());
    const size_t baseline = h.radio.hub_to_node_frames().size();

    h.rol.enable_timed_mode(false);
    EXPECT_GT(h.radio.hub_to_node_frames().size(), baseline)
        << "a withdrawn grid must reach the node even while its session is rebuilt";
}

// —————————————————————————————————————-
// MAC-2's cost can only be attributed via turnaround if the ping the hub
// SENDS is actually encrypted when the operator asks for it. Before this,
// start_mac_ping had no crypto option at all — build_mac_ping_frame_ packed
// raw, unconditionally — so a "MAC-1 + MAC-2" turnaround run would have
// measured MAC-0 no matter what the node's sublayer switches said.
//
// The msgid for an encrypted ping cannot be the dedicated ping counter: the
// AAD/IV are derived from the OUTER header's msgid under this node's base
// nonce, and reusing a value already used for an ordinary command would be a
// GCM nonce reuse. So the reserved-block mechanism is tested directly.
//
// Uses the REAL protobuf-c structs (as seam_test.cpp and the TimeSync check
// above do), not the proto_sim:: hand mirror — that mirror's Cmd enum has no
// MacControl case at all.
// —————————————————————————————————————-
namespace {
struct TxIdProbe : LORAClient {
    uint32_t txId() const { return this->frame_counter_.tx_message_id; }
};

// Decrypt a hub->node frame with `base_nonce` and unpack it as the REAL
// protobuf-c struct. Returns nullptr if the frame is not CMD_ENCRYPTED or the
// tag does not verify. Caller frees with lora_client_operation_message__free_unpacked.
LoraClientOperationMessage *decrypt_downlink_real(const std::vector<uint8_t> &bytes,
                                                  uint32_t base_nonce,
                                                  uint32_t *out_msgid = nullptr) {
    LoraClientOperationMessage *outer =
        lora_client_operation_message__unpack(nullptr, bytes.size(), bytes.data());
    if (outer == nullptr) return nullptr;
    if (outer->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED || outer->encrypted == nullptr ||
        outer->header == nullptr) {
        lora_client_operation_message__free_unpacked(outer, nullptr);
        return nullptr;
    }
    framecrypto::EtmHeaderFields fields{};
    fields.downlink       = true;
    fields.session_id     = base_nonce;
    fields.dest_address   = outer->header->destaddress;
    fields.dest_subnet    = outer->header->destsubnet;
    fields.sender_address = outer->header->senderaddress;
    fields.msgid          = outer->header->msgid;
    fields.burst_index    = outer->header->burstindex;
    fields.burst_count    = outer->header->burstcount;
    fields.on_mark        = outer->header->onmark;
    fields.fire_stamped   = outer->header->firestamped;
    fields.fire_round     = outer->header->fireround;
    fields.fire_offset_us = outer->header->fireoffsetus;
    auto plain = proto_sim::encrypt_then_cmac_open(
        base_nonce, kTestNodeNonce, esphome::lora_tracker::kHubAddress,
        static_cast<uint8_t>(outer->header->destaddress), /*downlink=*/true, fields,
        outer->encrypted->ciphertext.data, outer->encrypted->ciphertext.len,
        outer->encrypted->tag.data, outer->encrypted->tag.len);
    if (out_msgid != nullptr)
        *out_msgid = outer->header->msgid;
    lora_client_operation_message__free_unpacked(outer, nullptr);
    if (!plain) return nullptr;
    LoraClientOperationMessage *inner =
        lora_client_operation_message__unpack(nullptr, plain->size(), plain->data());
    // The inner message carries no header (stripped at encrypt time); the caller
    // reads the outer msgid via out_msgid where it matters.
    return inner;
}
}  // namespace

TEST(MacPingCrypto, AnEncryptedPingReachesTheNodeAndCarriesItsOwnSeq) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    const uint32_t base = drive_session(h.clock, h.radio, h.rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(h.rol.session_confirmed_);

    h.rol.start_mac_ping(/*duration_s=*/5, /*grid_ms=*/1000, /*want_echo=*/true,
                         /*pad_bytes=*/0, /*crypto=*/true);
    h.rol.mac_ping_tick_for_test();

    int plaintext_pings = 0, encrypted_pings = 0;
    uint32_t seq_seen = 0;
    for (const auto &f : h.radio.hub_to_node_frames()) {
        LoraClientOperationMessage *plain =
            lora_client_operation_message__unpack(nullptr, f.bytes.size(), f.bytes.data());
        if (plain != nullptr) {
            if (plain->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL)
                ++plaintext_pings;
            lora_client_operation_message__free_unpacked(plain, nullptr);
        }
        LoraClientOperationMessage *inner = decrypt_downlink_real(f.bytes, base);
        if (inner != nullptr) {
            if (inner->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL &&
                inner->maccontrol != nullptr) {
                ++encrypted_pings;
                seq_seen = inner->maccontrol->seq;
            }
            lora_client_operation_message__free_unpacked(inner, nullptr);
        }
    }
    EXPECT_EQ(plaintext_pings, 0) << "with crypto requested, the ping must not go out in the clear";
    EXPECT_EQ(encrypted_pings, 1) << "and it must actually be delivered, encrypted";
    EXPECT_EQ(seq_seen, 1u) << "the ping's own seq is unaffected by which msgid space it borrows";
}

TEST(MacPingCrypto, WithoutASessionCryptoFallsBackToPlaintextLikeEveryOtherDownlink) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    ASSERT_FALSE(h.rol.session_confirmed_);
    const uint32_t txid_before = static_cast<TxIdProbe &>(h.rol).txId();

    h.rol.start_mac_ping(/*duration_s=*/5, /*grid_ms=*/1000, /*want_echo=*/true,
                         /*pad_bytes=*/0, /*crypto=*/true);
    EXPECT_EQ(static_cast<TxIdProbe &>(h.rol).txId(), txid_before)
        << "no session means nothing will ever be encrypted this run, so no "
           "msgid block may be reserved (and no NVS write made) for it";
    h.rol.mac_ping_tick_for_test();

    int plaintext_pings = 0;
    for (const auto &f : h.radio.hub_to_node_frames()) {
        LoraClientOperationMessage *m =
            lora_client_operation_message__unpack(nullptr, f.bytes.size(), f.bytes.data());
        if (m != nullptr) {
            if (m->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL)
                ++plaintext_pings;
            lora_client_operation_message__free_unpacked(m, nullptr);
        }
    }
    EXPECT_EQ(plaintext_pings, 1)
        << "no base nonce exists without a session, so this cannot be sealed — "
           "plaintext is the safe fallback, not a stall";
}

TEST(MacPingCrypto, TheReservedMsgidRangeNeverCollidesWithAnOrdinaryCommand) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    const uint32_t base = drive_session(h.clock, h.radio, h.rol);
    ASSERT_NE(base, 0u);
    const uint32_t txid_before_reserve = static_cast<TxIdProbe &>(h.rol).txId();

    // A short run at a fast grid: few frames, so the reserved block is small
    // and easy to reason about, but still nonzero.
    h.rol.start_mac_ping(/*duration_s=*/2, /*grid_ms=*/500, /*want_echo=*/false,
                         /*pad_bytes=*/0, /*crypto=*/true);
    const uint32_t txid_after_reserve = static_cast<TxIdProbe &>(h.rol).txId();
    EXPECT_GT(txid_after_reserve, txid_before_reserve)
        << "the WHOLE run's msgids must be reserved at start_mac_ping, before the "
           "esp_timer callback ever runs — not incrementally per frame, which "
           "would be an NVS write per frame from that task";

    for (int i = 0; i < 4; ++i)
        h.rol.mac_ping_tick_for_test();

    // An ORDINARY command, through the normal encrypted path, right after.
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.5f);
    const uint32_t ordinary_msgid = static_cast<TxIdProbe &>(h.rol).txId();
    EXPECT_GT(ordinary_msgid, txid_after_reserve)
        << "an ordinary command sent during (or after) the ping run must land "
           "STRICTLY past the reserved block — inside it would be the exact "
           "nonce reuse the reservation exists to prevent";

    // And every ping actually sent decrypts distinctly under the one base
    // nonce, USING a msgid from the reserved block — not the dedicated ping
    // counter (which would also start small and decrypt "successfully" against
    // itself, since encryption and decryption both read whatever msgid the
    // header actually carries; only comparing it against the reservation
    // catches the wrong counter being used).
    int pings_decrypted = 0;
    for (const auto &f : h.radio.hub_to_node_frames()) {
        uint32_t msgid = 0;
        LoraClientOperationMessage *inner = decrypt_downlink_real(f.bytes, base, &msgid);
        if (inner != nullptr) {
            if (inner->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL) {
                ++pings_decrypted;
                EXPECT_GT(msgid, txid_before_reserve)
                    << "an encrypted ping's msgid must come from the RESERVED block, "
                       "not the dedicated ping counter (which starts at 1 regardless)";
                EXPECT_LE(msgid, txid_after_reserve);
            }
            lora_client_operation_message__free_unpacked(inner, nullptr);
        }
    }
    EXPECT_EQ(pings_decrypted, 4) << "four distinct pings, all decrypting under the one base nonce";
}

TEST(MacPingCrypto, EndingAnUnfinishedCryptoRunDoesNotLeakItsRangeIntoTheNextOne) {
    // A crypto run stopped early (or one whose estimate overshot) leaves its
    // reserved range PARTLY unused. A later plaintext run must not be able to
    // read as "still encrypting" from that leftover state.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;
    const uint32_t base = drive_session(h.clock, h.radio, h.rol);
    ASSERT_NE(base, 0u);

    h.rol.start_mac_ping(/*duration_s=*/5, /*grid_ms=*/1000, /*want_echo=*/false,
                         /*pad_bytes=*/0, /*crypto=*/true);
    h.rol.mac_ping_tick_for_test();   // one of several reserved frames used
    h.rol.stop_mac_ping();

    // hub_to_node_frames() returns BY VALUE: snapshot it once rather than
    // indexing repeated calls, which would index into a fresh temporary each
    // time and read a dangling reference.
    const size_t before = h.radio.hub_to_node_frames().size();
    h.rol.start_mac_ping(/*duration_s=*/5, /*grid_ms=*/1000, /*want_echo=*/false,
                         /*pad_bytes=*/0, /*crypto=*/false);
    h.rol.mac_ping_tick_for_test();
    const auto frames = h.radio.hub_to_node_frames();

    int plaintext = 0, encrypted = 0;
    for (size_t i = before; i < frames.size(); ++i) {
        const auto &f = frames[i];
        LoraClientOperationMessage *m =
            lora_client_operation_message__unpack(nullptr, f.bytes.size(), f.bytes.data());
        if (m != nullptr) {
            if (m->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL) ++plaintext;
            lora_client_operation_message__free_unpacked(m, nullptr);
        }
        LoraClientOperationMessage *inner = decrypt_downlink_real(f.bytes, base);
        if (inner != nullptr) {
            if (inner->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL) ++encrypted;
            lora_client_operation_message__free_unpacked(inner, nullptr);
        }
    }
    EXPECT_EQ(plaintext, 1) << "the second run asked for plaintext";
    EXPECT_EQ(encrypted, 0)
        << "the first run's unused reserved msgids must not leak into a run "
           "that never asked to be encrypted";
}

TEST(RealLoraClient, AnAutomaticNodesTimeSyncIsThrottledToOnceAWeek) {
    // power-rf-review-2026-09-27.md finding 1, narrowed to Mode C only per user
    // decision: Mode B and plain interactive nodes keep today's unthrottled
    // per-wake TimeSync unchanged; only an automatic-mode node's resync moves
    // to once a week.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();
    proto_sim_timer_set_now_us(1'000'000);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.set_auto_mode_default(true);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    clock.tick(1000);   // past the 750 ms deferred push

    auto count_timesync = [&]() {
        int n = 0;
        for (const auto& f : radio.hub_to_node_frames()) {
            auto inner = decrypt_downlink(f, base);
            if (inner && inner->cmd == proto_sim::LoraClientOperationMessage::Cmd::TimeSync) ++n;
        }
        return n;
    };
    ASSERT_EQ(count_timesync(), 1) << "the first TimeSync for a node must always go through";
    ASSERT_GT(rol.last_timesync_sent_us_for_test(), 0);

    // A second wake, well short of a week: no new TimeSync.
    proto_sim_timer_advance_us(3600LL * 1'000'000);   // 1 hour
    rol.send_timesync();
    EXPECT_EQ(count_timesync(), 1) << "an automatic node's resync is not due for a week";

    // Past a week: due again.
    proto_sim_timer_advance_us(timesyncpolicy::kAutoModeIntervalUs);
    rol.send_timesync();
    EXPECT_EQ(count_timesync(), 2) << "a week later the resync is due";
}

TEST(RealLoraClient, ModeBAndPlainInteractiveTimeSyncIsUnthrottled) {
    // The user's explicit instruction: leave Mode B (and everything that is
    // not automatic mode) exactly as it was.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();
    proto_sim_timer_set_now_us(1'000'000);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    ASSERT_FALSE(rol.get_auto_mode());   // precondition: not automatic

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    clock.tick(1000);   // past the 750 ms deferred push

    auto count_timesync = [&]() {
        int n = 0;
        for (const auto& f : radio.hub_to_node_frames()) {
            auto inner = decrypt_downlink(f, base);
            if (inner && inner->cmd == proto_sim::LoraClientOperationMessage::Cmd::TimeSync) ++n;
        }
        return n;
    };
    ASSERT_EQ(count_timesync(), 1);

    proto_sim_timer_advance_us(1000);   // a fraction of a second later
    rol.send_timesync();
    EXPECT_EQ(count_timesync(), 2)
        << "unchanged behaviour: no throttling outside automatic mode, however soon it repeats";
}

// ---------------------------------------------------------------------------
// A hub reboot must not force a REGISTER round trip for a node whose config
// has not changed (power-rf-review-2026-09-27.md finding 2, user decision:
// "if a register can be avoided let's avoid").
//
// config_synced_ used to be plain in-RAM state, reset to false by every hub
// process restart regardless of whether anything about the node's config had
// actually changed -- so send_login()'s request_register = !config_synced_
// fired on every node, every hub boot. It is now cross-checked against a hash
// of what ClientConfig actually carries, persisted alongside the rest of this
// node's NVS-restored state (LORAClientRestoreState, bumped to v3).
//
// Two SEPARATE LORAListener objects, deliberately NOT going through
// RealHubHarness (its constructor calls reset_nvs()) -- the whole point here
// is that NVS is NOT reset between them, exactly as it is not reset by an
// esphome process restart on real hardware.
// ---------------------------------------------------------------------------

TEST(ConfigHashPersistence, AnUnchangedConfigSkipsRegisterAcrossAHubReboot) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();   // the one reset: node 2's FIRST ever boot
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    // Session 1 ("before the reboot"): the node registers, the hub confirms a
    // session and pushes config, which is what really sets config_synced_ and
    // saves the hash in production. Reproduced directly here, since driving
    // the full REGISTER/LOGIN exchange twice is not what this test is about.
    {
        LORATracker tracker1;
        LORAClient  rol1;
        rol1.set_name("rol");
        rol1.set_short_address(18);
        rol1.set_subnet_address(2);
        rol1.set_sleep_duration(21600);
        rol1.set_battery_update_interval(3600);
        rol1.set_address(kMacRol2);
        tracker1.register_client(&rol1);
        rol1.setup();
        ASSERT_FALSE(rol1.config_synced_) << "precondition: nothing persisted yet";

        rol1.registered_    = true;
        rol1.config_synced_ = true;   // what a real confirmed config push sets
        rol1.save_state_for_test();
    }

    // Session 2 ("after the reboot"): a fresh object, same NVS backing, same
    // config. It must NOT have to wait for REGISTER to know it is in sync.
    {
        LORATracker tracker2;
        LORAClient  rol2;
        rol2.set_name("rol");
        rol2.set_short_address(18);
        rol2.set_subnet_address(2);
        rol2.set_sleep_duration(21600);
        rol2.set_battery_update_interval(3600);
        rol2.set_address(kMacRol2);
        tracker2.register_client(&rol2);
        rol2.setup();

        EXPECT_TRUE(rol2.config_synced_)
            << "an unchanged config must be recognised from the persisted hash alone";
    }

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(ConfigHashPersistence, AConfigEditIsStillDetectedAndStillPushed) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    {
        LORATracker tracker1;
        LORAClient  rol1;
        rol1.set_name("rol");
        rol1.set_short_address(18);
        rol1.set_subnet_address(2);
        rol1.set_sleep_duration(21600);
        rol1.set_address(kMacRol2);
        tracker1.register_client(&rol1);
        rol1.setup();
        rol1.registered_    = true;
        rol1.config_synced_ = true;
        rol1.save_state_for_test();
    }

    // Same node, but the YAML now asks for a different sleep duration -- a
    // real config edit between the two boots.
    {
        LORATracker tracker2;
        LORAClient  rol2;
        rol2.set_name("rol");
        rol2.set_short_address(18);
        rol2.set_subnet_address(2);
        rol2.set_sleep_duration(43200);   // changed
        rol2.set_address(kMacRol2);
        tracker2.register_client(&rol2);
        rol2.setup();

        EXPECT_FALSE(rol2.config_synced_)
            << "a real config change must still force the REGISTER round trip";
    }

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(ConfigHashPersistence, AChangedAddressIsAlsoDetected) {
    // sleep_duration_ above; every other ClientConfig field gets the same
    // one-field-changed check so none of them is silently left out of the hash.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    {
        LORATracker tracker1;
        LORAClient  rol1;
        rol1.set_name("rol");
        rol1.set_short_address(18);
        rol1.set_subnet_address(2);
        rol1.set_sleep_duration(21600);
        rol1.set_address(kMacRol2);
        tracker1.register_client(&rol1);
        rol1.setup();
        rol1.registered_    = true;
        rol1.config_synced_ = true;
        rol1.save_state_for_test();
    }
    {
        LORATracker tracker2;
        LORAClient  rol2;
        rol2.set_name("rol");
        rol2.set_short_address(19);   // changed
        rol2.set_subnet_address(2);
        rol2.set_sleep_duration(21600);
        rol2.set_address(kMacRol2);
        tracker2.register_client(&rol2);
        rol2.setup();

        EXPECT_FALSE(rol2.config_synced_) << "an address change must also be detected";
    }

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(ConfigHashPersistence, ANodeWithNothingPersistedIsUnaffected) {
    // The ordinary first-boot-ever path: restore_state_() fails outright, and
    // config_synced_ must stay exactly what it always defaulted to.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    tracker.register_client(&rol);
    rol.setup();

    EXPECT_FALSE(rol.config_synced_);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// ===========================================================================
// 2026-10-06 hub cleanup: K_root pin + the msgid / CTR-space guard
// (framecrypto::kMsgidCtrLimit), mirror of the node's guard.
// ===========================================================================

namespace {

std::string hex16(const uint8_t *b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
    return s;
}

size_t count_logins_since(proto_sim::SimRadio &radio, size_t from) {
    size_t n = 0;
    for (size_t i = from; i < radio.transcript().size(); ++i) {
        const auto &f = radio.transcript()[i];
        if (f.dir != proto_sim::AirFrame::Dir::HubToNode) continue;
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login) ++n;
    }
    return n;
}

// An encrypted uplink carrying `msgid`, sealed under `base` like the real node.
// `corrupt_tag` flips one tag bit so it fails the CMAC.
std::vector<uint8_t> hand_sealed_uplink(uint32_t base, uint32_t msgid, bool corrupt_tag) {
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = msgid;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
    inner.avail.available      = true;
    auto plain = proto_sim::serialize_resp_payload(inner);
    auto enc = seal_uplink_like_node(base, inner.header, plain);
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
    outer.encrypted.ciphertext = enc.ciphertext;
    if (corrupt_tag) outer.encrypted.tag[0] ^= 0x01;
    return proto_sim::serialize_resp(outer);
}

struct MsgidGuardFixture {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker tracker;
    LORAClient  rol;
    RealTimeClock time;
    uint32_t base{0};
    MsgidGuardFixture() {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        ensure_psa_ready();
        rol.set_name("rol");
        rol.set_short_address(18);
        rol.set_subnet_address(2);
        rol.set_sleep_duration(21600);
        rol.set_address(kMacRol2);
        time.set_now(1787000000, /*valid=*/true);
        rol.set_time(&time);
        tracker.register_client(&rol);
        base = drive_session(clock, radio, rol);
    }
    ~MsgidGuardFixture() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

}  // namespace

TEST(HubRootKey, IsSha256OfTheFleetKeyPhraseTruncatedTo16Bytes) {
    ensure_psa_ready();
    // The real_lora_client target is compiled with LORA_FLEET_KEY=
    // "ProtoSimTestKeyNotReal": SHA-256 of that phrase, first 16 bytes.
    uint8_t got[16] = {0};
    ASSERT_TRUE(LORAClient::deriveRootKeyForTest(got));
    EXPECT_EQ(hex16(got), "abfdd27c60308b655495436cef9f7c5f");

    // And the production dev literal's derivation, pinned independently of the
    // hub code: K_root = SHA-256("LoRaHome")[0:16]. (Computed with sha256sum.)
    const char *phrase = "LoRaHome";
    uint8_t hash[32];
    size_t  hash_len = 0;
    ASSERT_EQ(psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(phrase),
                               strlen(phrase), hash, sizeof(hash), &hash_len), PSA_SUCCESS);
    EXPECT_EQ(hex16(hash), "16efc5cc9770cb2cb8e84a8c9feff412");
}

TEST(MsgidGuard, TheTxCounterNeverHandsOutAMsgidBeyondTheCtrSpace) {
    MsgidGuardFixture f;
    ASSERT_NE(f.base, 0u);
    ASSERT_TRUE(f.rol.session_confirmed_);

    f.rol.frame_counter_.tx_message_id = 0x7FFFFFFDu;
    EXPECT_EQ(f.rol.incrTxMessageId(), 0x7FFFFFFEu);
    EXPECT_EQ(f.rol.incrTxMessageId(), 0x7FFFFFFFu) << "2^31-1 is the last usable id";
    EXPECT_FALSE(f.rol.msgidReloginPendingForTest());

    const size_t before = f.radio.transcript().size();
    const uint32_t got = f.rol.incrTxMessageId();
    EXPECT_EQ(got, framecrypto::kMsgidCtrLimit) << "the out-of-space marker";
    EXPECT_FALSE(framecrypto::msgidFitsCtr(got));
    EXPECT_EQ(f.rol.frame_counter_.tx_message_id, 0x7FFFFFFFu) << "the counter must not wrap or advance";
    EXPECT_TRUE(f.rol.msgidReloginPendingForTest());

    // A second refusal asks for nothing more, and the id is still refused.
    EXPECT_EQ(f.rol.incrTxMessageId(), framecrypto::kMsgidCtrLimit);
    EXPECT_EQ(f.rol.msgidReloginRequestsForTest(), 1u) << "a repeat refusal must not ask again";

    // The relogin goes out on the next tick: one LOGIN, counters restarted,
    // flag cleared (the exhaustion is over).
    EXPECT_EQ(count_logins_since(f.radio, before), 0u) << "deferred, not inline";
    f.clock.tick(200);
    EXPECT_EQ(count_logins_since(f.radio, before), 1u);
    EXPECT_FALSE(f.rol.msgidReloginPendingForTest());
    EXPECT_LT(f.rol.frame_counter_.tx_message_id, 0x100u) << "msgids start over";
}

TEST(MsgidGuard, TheSealPointRefusesAMsgidBeyondTheCtrSpaceAndAsksForARelogin) {
    MsgidGuardFixture f;
    ASSERT_TRUE(f.rol.session_confirmed_);

    LoraClientOperationMessage op;
    LoraHeader hdr;
    TimeSync ts;
    auto build = [&](uint32_t msgid) {
        hdr = LORA_HEADER__INIT;
        hdr.destaddress   = 18;
        hdr.destsubnet    = 2;
        hdr.senderaddress = esphome::lora_tracker::kHubAddress;
        hdr.msgid         = msgid;
        ts = TIME_SYNC__INIT;
        ts.epoch = 1787000000;
        op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
        op.header   = &hdr;
        op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC;
        op.timesync = &ts;
    };

    // CONTROL: an ordinary msgid seals.
    uint8_t *buf = nullptr;
    size_t len = 0;
    build(5);
    ASSERT_TRUE(f.rol.packOperationForTest(&op, true, &buf, &len)) << "control: an ordinary msgid seals";
    free(buf);
    ASSERT_FALSE(f.rol.msgidReloginPendingForTest());

    // 2^31 + 5 would reuse msgid 5's keystream.
    buf = nullptr;
    len = 0;
    build(0x80000005u);
    EXPECT_FALSE(f.rol.packOperationForTest(&op, true, &buf, &len)) << "must not seal";
    EXPECT_EQ(buf, nullptr);
    EXPECT_TRUE(f.rol.msgidReloginPendingForTest());

    // The same msgid in the clear is not a CTR problem (no keystream): the
    // guard applies to sealing only.
    f.rol.clearMsgidReloginForTest();
    buf = nullptr;
    len = 0;
    build(0x80000005u);
    EXPECT_TRUE(f.rol.packOperationForTest(&op, /*encrypt=*/false, &buf, &len));
    free(buf);
    EXPECT_FALSE(f.rol.msgidReloginPendingForTest());
}

TEST(MsgidGuard, TheRetagPointRefusesAMsgidBeyondTheCtrSpace) {
    MsgidGuardFixture f;
    f.rol.mark_session_confirmed_for_test();
    f.rol.send_remote_config();   // snapshots the seal generation

    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = 18;
    hdr.destsubnet    = 2;
    hdr.senderaddress = esphome::lora_tracker::kHubAddress;
    hdr.msgid         = 999;
    uint8_t ciphertext[4] = {1, 2, 3, 4};
    uint8_t tag[framecrypto::kSessionCmacTagBytes] = {0};
    EncryptedPayload enc = ENCRYPTED_PAYLOAD__INIT;
    enc.ciphertext.data = ciphertext;
    enc.ciphertext.len  = sizeof(ciphertext);
    enc.tag.data        = tag;
    enc.tag.len         = sizeof(tag);

    ASSERT_TRUE(f.rol.sealBurstCopyTag(&enc, &hdr)) << "control: an ordinary msgid is retagged";

    memset(tag, 0, sizeof(tag));
    hdr.msgid = 0x80000000u + 999;
    EXPECT_FALSE(f.rol.sealBurstCopyTag(&enc, &hdr)) << "msgid beyond the CTR space is not tagged";
    EXPECT_EQ(f.rol.staleSessionDropsForTest(), 0u) << "refused for the msgid, not as a stale session";
    for (uint8_t b : tag) EXPECT_EQ(b, 0) << "the tag bytes must be untouched";
}

// Characterisation (CCN refactor of sealBurstCopyTag): the written tag is
// EXACTLY the independent oracle's CMAC over the header fields and the
// ciphertext, so every field the retag point must cover is shown to reach the
// MAC input; and every refusal leaves the caller's tag bytes alone.
namespace {
struct RetagCase {
    LoraHeader hdr = LORA_HEADER__INIT;
    uint8_t ct[5] = {9, 8, 7, 6, 5};
    uint8_t tag[framecrypto::kSessionCmacTagBytes] = {0};
    EncryptedPayload enc = ENCRYPTED_PAYLOAD__INIT;
    RetagCase() {
        hdr.destaddress = 18; hdr.destsubnet = 2;
        hdr.senderaddress = esphome::lora_tracker::kHubAddress;
        hdr.msgid = 999; hdr.burstindex = 3; hdr.burstcount = 17;
        hdr.onmark = true; hdr.firestamped = true;
        hdr.fireround = 0x1234; hdr.fireoffsetus = 4567;
        enc.ciphertext.data = ct; enc.ciphertext.len = sizeof(ct);
        enc.tag.data = tag; enc.tag.len = sizeof(tag);
    }
    void oracle(uint32_t session_id, uint8_t out[framecrypto::kSessionCmacTagBytes]) const {
        framecrypto::EtmHeaderFields fx{};
        fx.downlink = true; fx.session_id = session_id;
        fx.dest_address = hdr.destaddress; fx.dest_subnet = hdr.destsubnet;
        fx.sender_address = hdr.senderaddress; fx.msgid = hdr.msgid;
        fx.burst_index = hdr.burstindex; fx.burst_count = hdr.burstcount;
        fx.on_mark = hdr.onmark; fx.fire_stamped = hdr.firestamped;
        fx.fire_round = hdr.fireround; fx.fire_offset_us = hdr.fireoffsetus;
        proto_sim::encrypt_then_cmac_retag(session_id, kTestNodeNonce,
                                           esphome::lora_tracker::kHubAddress, 18,
                                           /*downlink=*/true, fx, ct, enc.ciphertext.len, out);
    }
};
}  // namespace

TEST(MsgidGuard, TheRetagIsExactlyTheCmacOverEveryBurstField) {
    MsgidGuardFixture f;
    f.rol.mark_session_confirmed_for_test();
    f.rol.send_remote_config();   // snapshots the seal generation

    auto expectMatches = [&](RetagCase &c, const char *what) {
        memset(c.tag, 0xAA, sizeof(c.tag));
        uint8_t want[framecrypto::kSessionCmacTagBytes];
        c.oracle(f.base, want);
        ASSERT_TRUE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << what;
        EXPECT_EQ(0, memcmp(c.tag, want, sizeof(want))) << what;
    };
    RetagCase base;
    expectMatches(base, "baseline");
    uint8_t baseline_tag[framecrypto::kSessionCmacTagBytes];
    memcpy(baseline_tag, base.tag, sizeof(baseline_tag));

    // One field at a time: the tag follows it (oracle agrees) and moves.
    auto vary = [&](const char *what, auto mutate) {
        RetagCase c; mutate(c);
        expectMatches(c, what);
        EXPECT_NE(0, memcmp(c.tag, baseline_tag, sizeof(baseline_tag))) << what << " must change the tag";
    };
    vary("destsubnet",   [](RetagCase &c) { c.hdr.destsubnet = 3; });
    vary("senderaddress",[](RetagCase &c) { c.hdr.senderaddress = 9; });
    vary("msgid",        [](RetagCase &c) { c.hdr.msgid = 1000; });
    vary("burstindex",   [](RetagCase &c) { c.hdr.burstindex = 4; });
    vary("burstcount",   [](RetagCase &c) { c.hdr.burstcount = 16; });
    vary("onmark",       [](RetagCase &c) { c.hdr.onmark = false; });
    vary("firestamped",  [](RetagCase &c) { c.hdr.firestamped = false; });
    vary("fireround",    [](RetagCase &c) { c.hdr.fireround = 0x1235; });
    vary("fireoffsetus", [](RetagCase &c) { c.hdr.fireoffsetus = 4568; });
    vary("ciphertext",   [](RetagCase &c) { c.ct[4] ^= 1; });
    vary("ciphertext length", [](RetagCase &c) { c.enc.ciphertext.len = 4; });
}

TEST(MsgidGuard, EveryRetagRefusalLeavesTheTagBytesAlone) {
    MsgidGuardFixture f;
    f.rol.mark_session_confirmed_for_test();
    f.rol.send_remote_config();

    auto untouched = [](const RetagCase &c) {
        for (uint8_t b : c.tag) EXPECT_EQ(b, 0xAA);
    };
    { RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag));
      ASSERT_TRUE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << "control"; }

    { RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag));
      EXPECT_FALSE(f.rol.sealBurstCopyTag(nullptr, &c.hdr)); untouched(c); }
    { RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag));
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, nullptr)); untouched(c); }
    { RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag)); c.enc.tag.len = sizeof(c.tag) - 1;
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << "wrong tag length"; untouched(c); }
    { RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag)); c.enc.tag.len = sizeof(c.tag) + 1;
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << "wrong tag length"; untouched(c); }
    { RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag)); c.enc.tag.data = nullptr;
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << "no tag buffer"; untouched(c); }
    { RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag)); c.hdr.msgid = 0x80000000u;
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << "beyond the CTR space"; untouched(c); }
    { // a destination that was never sealed under: generation lookup misses -> stale
      RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag)); c.hdr.destaddress = 77;
      const uint32_t before = f.rol.staleSessionDropsForTest();
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr));
      EXPECT_EQ(f.rol.staleSessionDropsForTest(), before + 1) << "counted as a stale-session drop";
      untouched(c); }
    { // no MAC key installed: not stale, not tagged, not counted
      RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag));
      const psa_key_id_t saved = f.rol.k_mac_key_id_;
      const uint32_t before = f.rol.staleSessionDropsForTest();
      f.rol.k_mac_key_id_ = PSA_KEY_ID_NULL;
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << "no K_mac";
      f.rol.k_mac_key_id_ = saved;
      EXPECT_EQ(f.rol.staleSessionDropsForTest(), before);
      untouched(c); }
    { // a MAC key id PSA does not know: psa_mac_compute fails, nothing is written
      RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag));
      const psa_key_id_t saved = f.rol.k_mac_key_id_;
      const uint32_t before = f.rol.staleSessionDropsForTest();
      f.rol.k_mac_key_id_ = static_cast<psa_key_id_t>(0x7FFFFFF0u);
      EXPECT_FALSE(f.rol.sealBurstCopyTag(&c.enc, &c.hdr)) << "PSA rejects the key";
      f.rol.k_mac_key_id_ = saved;
      EXPECT_EQ(f.rol.staleSessionDropsForTest(), before);
      untouched(c); }
    { // a peer this hub has no session id for
      LORAClient other; other.set_name("other"); other.set_short_address(77);
      RetagCase c; memset(c.tag, 0xAA, sizeof(c.tag));
      EXPECT_FALSE(other.sealBurstCopyTag(&c.enc, &c.hdr)) << "no session id for the peer";
      untouched(c); }
}

TEST(MsgidGuard, AnAuthenticUplinkBeyondTheCtrSpaceIsRefusedAndStartsAFreshSession) {
    MsgidGuardFixture f;
    ASSERT_TRUE(f.rol.login_acked_);
    f.rol.frame_counter_.rx_message_id = 0x7FFFFFF0u;

    // CONTROL: an ordinary authentic uplink is accepted and asks for nothing.
    auto ok = hand_sealed_uplink(f.base, 0x7FFFFFF1u, false);
    f.rol.set_response(ok.data(), ok.size());
    ASSERT_EQ(f.rol.frame_counter_.rx_message_id, 0x7FFFFFF1u) << "precondition: the control frame was accepted";
    ASSERT_FALSE(f.rol.msgidReloginPendingForTest());

    // A FORGED frame (one tag bit flipped) with a huge msgid is dropped on the
    // tag, before the msgid guard can act on a value an attacker chose.
    auto forged = hand_sealed_uplink(f.base, 0x80000003u, true);
    f.rol.set_response(forged.data(), forged.size());
    EXPECT_FALSE(f.rol.msgidReloginPendingForTest()) << "an unauthenticated frame must not trigger a relogin";
    EXPECT_EQ(f.rol.frame_counter_.rx_message_id, 0x7FFFFFF1u);

    // The authentic one: refused, replay counter untouched, fresh session asked for.
    const size_t before = f.radio.transcript().size();
    auto bad = hand_sealed_uplink(f.base, 0x80000003u, false);
    f.rol.set_response(bad.data(), bad.size());
    EXPECT_EQ(f.rol.frame_counter_.rx_message_id, 0x7FFFFFF1u) << "must not advance the replay counter";
    EXPECT_TRUE(f.rol.msgidReloginPendingForTest());

    // A second one before the login goes out does not queue a second login.
    auto bad2 = hand_sealed_uplink(f.base, 0x80000004u, false);
    f.rol.set_response(bad2.data(), bad2.size());

    EXPECT_EQ(f.rol.msgidReloginRequestsForTest(), 1u) << "once per exhaustion, not once per refused frame";
    f.clock.tick(200);
    EXPECT_EQ(count_logins_since(f.radio, before), 1u) << "once per exhaustion";
    EXPECT_FALSE(f.rol.msgidReloginPendingForTest());
}

// ===========================================================================
// OPTIMISTIC SINGLE SHOT, END TO END (2026-10-06)
//
// The switch (loradevices.yml, default OFF) lets the hub spend ONE copy on a
// node whose only refusal is an aged-out confirmation. These tests pin the
// half that makes a wrong guess cheap: the FIRST ack wait is short, measured
// from the placed mark; the burst fallback still happens; and the miss is not
// charged against the retry budget that ends in a session teardown.
// ===========================================================================

namespace {

// The listener's tracked-op bookkeeping is protected; same probe pattern as
// AckProbe above, adding no state.
struct OptProbe : LORAClient {
    using esphome::lora_tracker::LORAListener::last_placed_t0_us_;
    using esphome::lora_tracker::LORAListener::op_frame_;
    using esphome::lora_tracker::LORAListener::op_first_msgid_;
    using esphome::lora_tracker::LORAListener::relogin_pending_;
    using esphome::lora_tracker::LORAListener::last_timesync_sent_us_;
    using esphome::lora_tracker::LORAListener::op_deferred_until_login_;
};
OptProbe &P(real_helpers::RealHubHarness &h) { return static_cast<OptProbe &>(h.rol); }

// Grid confirmed, a good phase report, then 24 h of silence: the ONLY thing
// refusing single shot is ConfirmationStale — asserted, not assumed.
void staleButOtherwiseGood(real_helpers::RealHubHarness &h) {
    confirmedGrid(h);
    h.rol.node_fw_version_ = 10104;
    h.rol.notePhaseReportForTest(/*rtc_slow_src=*/2, /*err_us=*/500,
                                 /*spread_us=*/800, /*samples=*/8,
                                 /*outside_guard=*/0, /*node_timed_rx=*/true);
    proto_sim_timer_advance_us((int64_t) 24 * 60 * 60 * 1'000'000LL);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::ConfirmationStale)
        << "precondition: staleness, and nothing else, is what refuses";
}

void sendOpen(real_helpers::RealHubHarness &h) {
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
}

// Microseconds from now to the mark the last command was placed on.
int64_t usToPlacedMark(real_helpers::RealHubHarness &h) {
    return P(h).last_placed_t0_us_ - esp_timer_get_time();
}

uint32_t nowMs() { return (uint32_t) (esp_timer_get_time() / 1000); }

// The first-ack wait the listener will arm for the command just sent, as the
// pure policy computes it from this node's own history (tx drain is 0 in the
// shim unless a test sets it).
uint32_t expectedWaitMs(real_helpers::RealHubHarness &h) {
    return singleshotwait::firstAckWaitMs(usToPlacedMark(h), 0,
                                          h.rol.ack_latency_for_test().tailMs(nowMs()));
}

// Give THIS node `n` clean mark->ack samples of `delay_ms`.
void seedLatency(real_helpers::RealHubHarness &h, uint32_t delay_ms, unsigned n = 3) {
    for (unsigned i = 0; i < n; ++i)
        h.rol.ack_latency_for_test().note(nowMs(), delay_ms);
}

// Ms from now until a fallback burst leaves (10 ms steps, bounded).
int msUntilBurst(real_helpers::RealHubHarness &h, int limit_ms = 12000) {
    const size_t sends = h.tracker.sent_copies.size();
    for (int t = 0; t < limit_ms; t += 10) {
        h.clock.tick(10);
        if (h.tracker.sent_copies.size() != sends) return t + 10;
    }
    return -1;
}

}  // namespace

TEST(OptimisticShot, OnAndStaleSendsOneCopyAndRemembersTheGuessWasOptimistic) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None);

    sendOpen(h);
    ASSERT_TRUE(h.rol.awaitingAck());
    EXPECT_EQ(h.tracker.last_copies, 1) << "one placed copy, not a 17-copy burst";
    EXPECT_TRUE(h.rol.op_sent_optimistic_for_test())
        << "the bypass was what made it a single shot";
    EXPECT_LE(P(h).op_frame_.size(), singleshotwait::kLargestTrackedOpBytes)
        << "the tail budget assumes a tracked op this small";
    EXPECT_EQ((uint32_t) LORAClient::kUplinkOffsetUs, singleshotwait::kNodeReplyOffsetUs)
        << "SingleShotAckWait.h restates the node's reply offset";
}

TEST(OptimisticShot, SwitchOffBurstsAsBeforeAndWaitsTheFullInterval) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    ASSERT_FALSE(h.rol.optimistic_single_shot());

    sendOpen(h);
    EXPECT_NE(h.tracker.last_copies, 1) << "switch off: the burst it always was";
    EXPECT_FALSE(h.rol.op_sent_optimistic_for_test());

    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(2000);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends) << "no early retransmit";
    EXPECT_EQ(h.rol.opRetryCount(), 0u);
    h.clock.tick(1000 + 50);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends + 1) << "the normal 3 s retry";
    EXPECT_EQ(h.rol.opRetryCount(), 1u);
}

TEST(OptimisticShot, AMissedFirstShotBurstsAfterTheShortWaitNotAfterThreeSeconds) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);

    const uint32_t wait_ms = expectedWaitMs(h);
    ASSERT_GE(wait_ms, singleshotwait::kFloorTailMs);

    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(wait_ms - 30);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends) << "not before the wait has run out";
    EXPECT_FALSE(h.rol.hubBelief().single_shot_unacked);

    h.clock.tick(60);
    ASSERT_EQ(h.tracker.sent_copies.size(), sends + 1) << "the burst fallback went out";
    EXPECT_NE(h.tracker.last_copies, 1) << "and it is a BURST";
    EXPECT_TRUE(h.rol.hubBelief().single_shot_unacked) << "Rule 4 fired";
    EXPECT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::SingleShotUnacked);
    EXPECT_TRUE(h.rol.op_short_wait_spent_for_test());
    EXPECT_TRUE(h.rol.awaitingAck()) << "still waiting for the node";

    // The ack for the ORIGINAL msgid (the retry is the same command) lands.
    deliverAck(h.rol, P(h).op_first_msgid_);
    EXPECT_FALSE(h.rol.awaitingAck());
    EXPECT_FALSE(h.rol.commandFailed());
}

TEST(OptimisticShot, TheWaitIsMeasuredFromThePlacedMarkNotFromPlacement) {
    // A placed frame can be a whole round from the air. A flat 500 ms would
    // expire before the frame was ever sent and burst on top of it.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);

    // Put "now" about 1.2 s before one of this node's marks.
    const int64_t mark = h.tracker.nextT0ForSlotUs(h.rol.grid_slot(),
                                                   esp_timer_get_time() + 2'000'000);
    proto_sim_timer_set_now_us(mark - 1'200'000);

    seedLatency(h, 1700);   // tail 2125 ms
    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);
    const int64_t to_mark_us = usToPlacedMark(h);
    ASSERT_GT(to_mark_us, 900'000) << "precondition: the mark is genuinely far off";

    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(h.rol.op_first_tail_ms_for_test() + 200);   // where a mark-less tail would have fired
    EXPECT_EQ(h.tracker.sent_copies.size(), sends)
        << "the frame is still waiting for its mark; bursting now would collide with it";
    EXPECT_FALSE(h.rol.hubBelief().single_shot_unacked);

    h.clock.tick(singleshotwait::firstAckWaitMs(to_mark_us, 0, 2125) - h.rol.op_first_tail_ms_for_test() - 200 + 50);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends + 1) << "after the mark plus the tail";
}

TEST(OptimisticShot, ACancelledWaitDoesNothing) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);

    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(100);
    deliverAck(h.rol, P(h).op_first_msgid_);   // the guess was RIGHT
    h.clock.tick(5000);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends) << "an acked single shot is not followed by a burst";
    EXPECT_FALSE(h.rol.hubBelief().single_shot_unacked) << "and Rule 4 stays quiet";
    EXPECT_FALSE(h.rol.commandFailed());
}

TEST(OptimisticShot, TheMissIsNotChargedAgainstTheRetryBudget) {
    // A wrong guess is what optimism accepts. If it counted, one miss would
    // leave 3 retries instead of 4 before "command failed after retries —
    // clearing session, forcing re-login" tears down a healthy session.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);
    h.clock.tick(expectedWaitMs(h) + 50);
    ASSERT_TRUE(h.rol.op_short_wait_spent_for_test());
    ASSERT_EQ(h.rol.opRetryCount(), 0u) << "the fallback burst is not a counted retry";

    // The four normal retries are all still available...
    for (uint32_t i = 1; i <= 4; ++i) {
        h.clock.tick(3000 + 50);
        EXPECT_EQ(h.rol.opRetryCount(), i);
        EXPECT_FALSE(h.rol.commandFailed()) << "retry " << i << " of 4 is within budget";
    }
    // ...and only the fifth timeout is the failure.
    h.clock.tick(3000 + 50);
    EXPECT_TRUE(h.rol.commandFailed());
}

TEST(OptimisticShot, TheShortWaitIsPerCommandNotPerSession) {
    // A second optimistic command, after the first one's miss, earns the short
    // wait again: the "spent" flag belongs to the command, not the listener.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);

    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);
    h.clock.tick(expectedWaitMs(h) + 50);
    ASSERT_TRUE(h.rol.op_short_wait_spent_for_test());
    deliverAck(h.rol, P(h).op_first_msgid_);   // the burst got through
    ASSERT_FALSE(h.rol.awaitingAck());

    // The node answers a beacon again (fresh report), then it is stale again.
    h.rol.notePhaseReportForTest(2, 500, 800, 8, 0, true);
    proto_sim_timer_advance_us((int64_t) 24 * 60 * 60 * 1'000'000LL);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None)
        << "Rule 4 cleared by the fresh report, staleness bypassed by the switch";

    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1) << "optimistic again";
    EXPECT_FALSE(h.rol.op_short_wait_spent_for_test()) << "a new command starts unspent";
    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(expectedWaitMs(h) + 50);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends + 1) << "the short wait applies again";
}

TEST(OptimisticShot, ASingleShotTheHubWasEntitledToKeepsTheNormalWait) {
    // Fresh evidence, switch on: still a single shot, but not an OPTIMISTIC one,
    // so the short wait does not apply.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    confirmedGrid(h);
    h.rol.node_fw_version_ = 10104;
    h.rol.notePhaseReportForTest(2, 500, 800, 8, 0, true);   // fresh
    h.rol.enable_optimistic_single_shot(true);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None);

    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);
    EXPECT_FALSE(h.rol.op_sent_optimistic_for_test())
        << "the bypass was not used, so this is not the optimistic case";

    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(2500);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends) << "the normal 3 s wait";
    h.clock.tick(500 + 50);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends + 1);
    EXPECT_EQ(h.rol.opRetryCount(), 1u) << "and this one IS a counted retry";
}

TEST(OptimisticShot, ASessionChangeStillBurstsAndKeepsTheNormalWait) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None);

    h.rol.send_login();   // a new session: positive evidence the node moved
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::SessionChanged)
        << "optimism must not outrank a session change";

    P(h).relogin_pending_ = false;
    sendOpen(h);
    EXPECT_NE(h.tracker.last_copies, 1) << "a burst";
    EXPECT_FALSE(h.rol.op_sent_optimistic_for_test());
    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(1500);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends) << "normal wait: nothing at 1.5 s";
    h.clock.tick(1500);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends + 1);
}

TEST(OptimisticShot, ARebootStillBurstsAndKeepsTheNormalWait) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None);

    auto reg = serialize_register(kMacRol2);   // the node's own notice that it restarted
    h.rol.set_response(reg.data(), reg.size());
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::RebootedSinceConfirm);

    P(h).relogin_pending_ = false;
    sendOpen(h);
    EXPECT_NE(h.tracker.last_copies, 1);
    EXPECT_FALSE(h.rol.op_sent_optimistic_for_test());
    const size_t sends = h.tracker.sent_copies.size();
    h.clock.tick(1500);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends);
    h.clock.tick(1500);
    EXPECT_EQ(h.tracker.sent_copies.size(), sends + 1);
}

// ---------------------------------------------------------------------------
// The adaptive first-ack deadline (SingleShotAckWait.h, 2026-10-08).
//
// Field fact: the ack of a placed single copy reaches the hub 1.6 - 2.2 s AFTER
// THE MARK. A 500 ms tail burst 17 copies on top of 4 of 8 commands that were
// about to be answered.
// ---------------------------------------------------------------------------

TEST(AdaptiveAckWait, AnAckAtTheMeasuredFieldLatencyDoesNotBurst) {
    // The exact 19:21:39 case: mark 556 ms out, ack 1.7 s after the mark. With
    // no history the deadline is the normal one; with history it is the node's.
    using namespace real_helpers;
    for (int with_history = 0; with_history < 2; ++with_history) {
        RealHubHarness h{18, kMacRol2};
        staleButOtherwiseGood(h);
        h.rol.enable_optimistic_single_shot(true);
        if (with_history) seedLatency(h, 2100);
        sendOpen(h);
        ASSERT_EQ(h.tracker.last_copies, 1);
        const int64_t mark = P(h).last_placed_t0_us_;
        const size_t sends = h.tracker.sent_copies.size();

        // The old deadline (to mark + 500 ms, plus the double-counted drain)
        // would have fired here, ~1.0 - 2.0 s after the command.
        proto_sim_timer_set_now_us(mark + 1'700'000 - 1);
        h.clock.tick(1);
        ASSERT_EQ(h.tracker.sent_copies.size(), sends) << "no premature burst";
        EXPECT_FALSE(h.rol.op_short_wait_spent_for_test());
        deliverAck(h.rol, P(h).op_first_msgid_);
        h.clock.tick(6000);
        EXPECT_EQ(h.tracker.sent_copies.size(), sends) << "an answered command is never followed by a burst";
        EXPECT_FALSE(h.rol.hubBelief().single_shot_unacked);
        EXPECT_FALSE(h.rol.commandFailed());
        // And the clean ack taught the node's history its real latency.
        EXPECT_NEAR((double) h.rol.ack_latency_for_test().maxDelayMs(nowMs()),
                    with_history ? 2100.0 : 1700.0, 1.0);
    }
}

TEST(AdaptiveAckWait, ATrulyMissingAckBurstsAtTheAdaptiveDeadlineNotBefore) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    seedLatency(h, 1800);                       // tail = 1800 + 450 = 2250
    ASSERT_EQ(h.rol.ack_latency_for_test().tailMs(nowMs()), 2250u);
    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);
    EXPECT_EQ(h.rol.op_first_tail_ms_for_test(), 2250u);
    const uint32_t wait_ms = expectedWaitMs(h);
    const size_t sends = h.tracker.sent_copies.size();

    h.clock.tick(wait_ms - 40);
    ASSERT_EQ(h.tracker.sent_copies.size(), sends) << "not before the deadline";
    h.clock.tick(80);
    ASSERT_EQ(h.tracker.sent_copies.size(), sends + 1) << "the fallback burst";
    EXPECT_NE(h.tracker.last_copies, 1);
    EXPECT_TRUE(h.rol.op_short_wait_spent_for_test());
    EXPECT_EQ(h.rol.opRetryCount(), 0u) << "a miss is not a counted retry";
    // The proven-too-short tail is remembered: the NEXT deadline is longer.
    EXPECT_EQ(h.rol.ack_latency_for_test().maxDelayMs(nowMs()), 2250u);
    EXPECT_GT(h.rol.ack_latency_for_test().tailMs(nowMs()), 2250u);
}

TEST(AdaptiveAckWait, ANodeWithSlowHistoryWaitsLongerThanOneWithFastHistory) {
    using namespace real_helpers;
    int fast_ms = 0, slow_ms = 0;
    uint32_t fast_tail = 0, slow_tail = 0;
    for (int slow = 0; slow < 2; ++slow) {
        RealHubHarness h{18, kMacRol2};
        staleButOtherwiseGood(h);
        h.rol.enable_optimistic_single_shot(true);
        seedLatency(h, slow ? 2300 : 1100);
        // Same placement for both: the mark may differ, so compare ms after it.
        sendOpen(h);
        ASSERT_EQ(h.tracker.last_copies, 1);
        (slow ? slow_tail : fast_tail) = h.rol.op_first_tail_ms_for_test();
        const int to_mark_ms = (int) (usToPlacedMark(h) / 1000);
        const int burst_ms = msUntilBurst(h);
        ASSERT_GT(burst_ms, 0) << "the burst must come";
        (slow ? slow_ms : fast_ms) = burst_ms - to_mark_ms;   // ms AFTER the mark
    }
    EXPECT_LT(fast_tail, slow_tail);
    EXPECT_LT(fast_ms, slow_ms) << "the slow node is given longer before a burst";
    EXPECT_GE(fast_ms, (int) fast_tail) << "never earlier than the observed latency";
    EXPECT_GE(slow_ms, (int) slow_tail);
}

TEST(AdaptiveAckWait, TheDeadlineNeverUndercutsTheObservedLatency) {
    // For several latencies: an ack at exactly the slowest sample seen is in time.
    using namespace real_helpers;
    for (uint32_t lat : {900u, 1700u, 2200u, 2800u}) {
        RealHubHarness h{18, kMacRol2};
        staleButOtherwiseGood(h);
        h.rol.enable_optimistic_single_shot(true);
        seedLatency(h, lat);
        sendOpen(h);
        ASSERT_EQ(h.tracker.last_copies, 1);
        const size_t sends = h.tracker.sent_copies.size();
        proto_sim_timer_set_now_us(P(h).last_placed_t0_us_ + (int64_t) lat * 1000);
        h.clock.tick(1);
        EXPECT_EQ(h.tracker.sent_copies.size(), sends) << "latency " << lat;
        EXPECT_FALSE(h.rol.op_short_wait_spent_for_test()) << "latency " << lat;
    }
}

TEST(AdaptiveAckWait, OnlyACleanSinglePlacedCopyFeedsTheHistory) {
    using namespace real_helpers;
    {   // a burst command (a session change refuses single shot): not a placed
        // copy, so even though an OLD mark exists it is not this command's
        RealHubHarness h{18, kMacRol2};
        confirmedGrid(h);
        h.rol.node_fw_version_ = 10104;
        h.rol.notePhaseReportForTest(2, 500, 800, 8, 0, true);
        h.rol.send_login();
        P(h).relogin_pending_ = false;
        sendOpen(h);
        ASSERT_NE(h.tracker.last_copies, 1);
        h.clock.tick(500);
        proto_sim_timer_advance_us(2'500'000);   // esp_timer is separate from the scheduler clock
        deliverAck(h.rol, P(h).op_first_msgid_);
        EXPECT_EQ(h.rol.ack_latency_for_test().count(nowMs()), 0u);
    }
    {   // an optimistic shot that missed and was answered after the fallback burst
        RealHubHarness h{18, kMacRol2};
        staleButOtherwiseGood(h);
        h.rol.enable_optimistic_single_shot(true);
        sendOpen(h);
        h.clock.tick(expectedWaitMs(h) + 50);
        ASSERT_TRUE(h.rol.op_short_wait_spent_for_test());
        const unsigned before = h.rol.ack_latency_for_test().count(nowMs());
        proto_sim_timer_advance_us(3'100'000);   // a positive, plausible mark->ack delay
        deliverAck(h.rol, P(h).op_first_msgid_);
        EXPECT_EQ(h.rol.ack_latency_for_test().count(nowMs()), before)
            << "an ack after a burst says nothing about the single-shot path";
    }
    {   // a single shot the hub was ENTITLED to (not optimistic) feeds it too,
        // so the history exists before the switch is ever turned on
        RealHubHarness h{18, kMacRol2};
        confirmedGrid(h);
        h.rol.node_fw_version_ = 10104;
        h.rol.notePhaseReportForTest(2, 500, 800, 8, 0, true);
        h.rol.enable_optimistic_single_shot(true);
        sendOpen(h);
        ASSERT_EQ(h.tracker.last_copies, 1);
        ASSERT_FALSE(h.rol.op_sent_optimistic_for_test());
        proto_sim_timer_set_now_us(P(h).last_placed_t0_us_ + 1'650'000);
        deliverAck(h.rol, P(h).op_first_msgid_);
        EXPECT_EQ(h.rol.ack_latency_for_test().count(nowMs()), 1u);
        EXPECT_EQ(h.rol.ack_latency_for_test().maxDelayMs(nowMs()), 1650u);
    }
}

TEST(AdaptiveAckWait, TheQueueDrainAndTheMarkAreNotCountedTwice) {
    // The first version ran the wait through ack_wait_ms_(), which ADDS the
    // drain, while the drain already contains the time to the mark. A deep
    // queue (3 s here) must give "later of the two + tail", not a sum.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    staleButOtherwiseGood(h);
    h.rol.enable_optimistic_single_shot(true);
    seedLatency(h, 1700);                       // tail 2125
    h.tracker.tx_drain_us = 3'000'000;
    sendOpen(h);
    ASSERT_EQ(h.tracker.last_copies, 1);
    const int burst_ms = msUntilBurst(h);
    EXPECT_NEAR(burst_ms, 3000 + 2125, 60) << "drain (the later of drain and mark) + tail, once";
}

TEST(AdaptiveAckWait, EachListenerKeepsItsOwnHistory) {
    using namespace real_helpers;
    RealHubHarness a{18, kMacRol2};
    RealHubHarness b{18, kMacRol2};
    seedLatency(a, 2400);
    EXPECT_EQ(a.rol.ack_latency_for_test().count(nowMs()), 3u);
    EXPECT_EQ(b.rol.ack_latency_for_test().count(nowMs()), 0u) << "no sharing between nodes";
    EXPECT_EQ(b.rol.ack_latency_for_test().tailMs(nowMs()), singleshotwait::kDefaultTailMs);
}

// ---------------------------------------------------------------------------
// The quiet-interactive-node gap in beacon_overdue_ (found 2026-10-06)
// ---------------------------------------------------------------------------

TEST(BeaconMissed, AnAwakeQuietInteractiveNodeIsNotMissedAfterItsSleepCycle) {
    // An interactive node beacons on wake, boot, relogin and mode change —
    // never while idle. After it has woken (beacon newer than the hub's last
    // CMD_SLEEP) nothing predicts another sleep, so 6 h of silence is not a
    // missed check-in. It used to read as BeaconMissed, a rung the optimistic
    // switch cannot bypass, so the node bursted forever.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    h.rol.registered_ = true;
    h.rol.set_sleep_duration(21600);
    h.rol.last_sleep_epoch_ = 1000;            // hub put it to sleep at 1000
    h.rol.noteBeaconEpochForTest(1100);        // ...and a button woke it early
    h.time.set_now(1000 + 21600 + LORAClient::kBeaconOverdueGraceS + 5, /*valid=*/true);
    EXPECT_FALSE(h.rol.node_overdue())
        << "it woke after the sleep that predicted it; nothing says it should beacon again";
    EXPECT_FALSE(h.rol.hubBelief().beacon_missed);

    // Never put to sleep by the hub at all: same answer, however long it is quiet.
    h.rol.last_sleep_epoch_ = 0;
    h.rol.noteBeaconEpochForTest(1000);
    h.time.set_now(1000 + 7 * 3600, /*valid=*/true);
    EXPECT_FALSE(h.rol.node_overdue()) << "awake for 7 h with no sleep on record";
}

TEST(BeaconMissed, AnInteractiveNodeTheHubPutToSleepAndNeverHeardAgainIsStillMissed) {
    // The fix must not blunt the check it sits in: a COMMANDED sleep that the
    // node does not wake from is exactly what U-5 exists to see.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.set_sleep_duration(600);
    h.rol.noteBeaconEpochForTest(900);
    h.rol.last_sleep_epoch_ = 1000;            // commanded to sleep AFTER its last beacon
    h.time.set_now(1000 + 600 + LORAClient::kBeaconOverdueGraceS + 1, /*valid=*/true);
    EXPECT_TRUE(h.rol.node_overdue());
}

TEST(BeaconMissed, AnAutoModeNodeIsStillJudgedByItsCheckinInterval) {
    // Auto-mode nodes DO check in periodically; the clause must not touch them.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.set_auto_mode_default(true);
    h.rol.set_checkin_interval(600);
    h.rol.noteBeaconEpochForTest(1000);        // last_sleep_epoch_ == 0 < beacon
    h.time.set_now(1000 + 600 + LORAClient::kBeaconOverdueGraceS + 1, /*valid=*/true);
    EXPECT_TRUE(h.rol.node_overdue());
}

TEST(OptimisticShot, AQuietInteractiveNodeKeepsQualifyingForSingleShotPastSixHours) {
    // The consequence the gap had, end to end through the ladder.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.set_sleep_duration(21600);
    confirmedGrid(h);
    h.rol.node_fw_version_ = 10104;
    h.time.set_now(1000, /*valid=*/true);
    h.rol.noteBeaconEpochForTest(1000);
    h.rol.notePhaseReportForTest(2, 500, 800, 8, 0, true);
    h.rol.enable_optimistic_single_shot(true);
    ASSERT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None);

    h.time.set_now(1000 + 7 * 3600, /*valid=*/true);
    proto_sim_timer_advance_us((int64_t) 7 * 3600 * 1'000'000LL);
    EXPECT_EQ(h.rol.txRefusalNow(), timedmode::TxRefusal::None)
        << "7 h quiet: still single-shot eligible (stale confirmation bypassed, "
           "and no spurious BeaconMissed)";
}

// ---------------------------------------------------------------------------
// The shadow pending-mask inputs, read off a real listener
// ---------------------------------------------------------------------------

TEST(PendingShadowInputs, TheListenerReportsWhatItOwes) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    const int64_t now = esp_timer_get_time();

    // A fresh listener owes plenty: login not settled, config not pushed.
    pendingshadow::NodeInputs in = h.rol.pending_shadow_inputs(now);
    EXPECT_TRUE(in.relogin_pending) << "session not confirmed";
    EXPECT_TRUE(in.push_awaiting_ack) << "config not yet pushed this boot";
    EXPECT_TRUE(in.timed_mode_off);
    EXPECT_FALSE(in.latency_tolerant);
    EXPECT_FALSE(in.op_awaiting_ack);
    EXPECT_EQ(in.slot, (uint8_t) (h.rol.grid_slot() % timedgrid::kSlotCount));

    // Settle everything the listener could owe.
    h.rol.session_confirmed_ = true;
    P(h).relogin_pending_   = false;
    h.rol.config_synced_     = true;
    h.rol.enable_timed_mode(true);
    h.rol.set_auto_mode_default(true);
    h.rol.node_sched_version_ = h.rol.schedule_version();
    h.rol.sched_dirty_ = false;
    h.rol.sched_push_msgid_ = 0;
    P(h).last_timesync_sent_us_ = now - 1000;
    // A published GridSync nobody has confirmed yet is something the node is owed.
    // Set directly: whether enable_timed_mode() manages to pack one depends on
    // session-key state that is static across tests in one process.
    h.rol.gridsync_msgids_[0]    = 0x1234;
    h.rol.gridsync_msgid_count_  = 1;
    ASSERT_TRUE(h.rol.gridSyncAwaitingAck());
    in = h.rol.pending_shadow_inputs(now);
    EXPECT_TRUE(in.push_awaiting_ack) << "GridSync awaiting its ack";
    deliverAck(h.rol, 0x1234);
    ASSERT_FALSE(h.rol.gridSyncAwaitingAck());
    in = h.rol.pending_shadow_inputs(now);
    EXPECT_FALSE(in.relogin_pending);
    EXPECT_FALSE(in.push_awaiting_ack);
    // A schedule the node has not been sent yet is owed too.
    h.rol.node_sched_version_ ^= 1u;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "schedule pending";
    h.rol.node_sched_version_ ^= 1u;
    EXPECT_FALSE(h.rol.pending_shadow_inputs(now).push_awaiting_ack);
    EXPECT_FALSE(in.timed_mode_off);
    EXPECT_TRUE(in.latency_tolerant) << "automatic mode is the proxy for latency tolerant";
    EXPECT_FALSE(in.timesync_due) << "an auto-mode node was given one a second ago";
    EXPECT_EQ(in.since_last_traffic_us, -1) << "never heard, never addressed: unknown";
}

// Characterisation (CCN refactor of pending_shadow_inputs): every owed-push
// condition is a reason on its own, and the schedule push only counts while it
// still has retries left.
TEST(PendingShadowInputs, EachOwedPushConditionIsAReasonOnItsOwn) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    const int64_t now = esp_timer_get_time();
    auto settle = [&]() {
        h.rol.session_confirmed_ = true;
        P(h).relogin_pending_    = false;
        h.rol.config_synced_     = true;
        h.rol.config_push_pending_ = false;
        h.rol.node_sched_version_ = h.rol.schedule_version();
        h.rol.sched_dirty_       = false;
        h.rol.sched_push_msgid_  = 0;
        h.rol.sched_push_retries_ = 0;
        h.rol.gridsync_msgid_count_ = 0;
    };
    settle();
    ASSERT_FALSE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "precondition";

    h.rol.sched_push_msgid_ = 7;
    h.rol.sched_push_retries_ = 0;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "push in flight";
    h.rol.sched_push_retries_ = LORAClient::kSchedMaxRetries - 1;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "one retry left";
    h.rol.sched_push_retries_ = LORAClient::kSchedMaxRetries;
    EXPECT_FALSE(h.rol.pending_shadow_inputs(now).push_awaiting_ack)
        << "a push that has used every retry is no longer owed";
    h.rol.sched_push_msgid_ = 0;
    h.rol.sched_push_retries_ = 0;
    EXPECT_FALSE(h.rol.pending_shadow_inputs(now).push_awaiting_ack);

    settle(); h.rol.sched_dirty_ = true;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "edit not yet pushed";
    settle(); h.rol.config_synced_ = false;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "config not synced";
    settle(); h.rol.config_push_pending_ = true;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "config push pending";
    settle(); h.rol.gridsync_msgid_count_ = 1;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "GridSync unconfirmed";
    settle(); h.rol.node_sched_version_ ^= 1u;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).push_awaiting_ack) << "schedule pending";
    settle();
    EXPECT_FALSE(h.rol.pending_shadow_inputs(now).push_awaiting_ack);
    EXPECT_FALSE(h.rol.pending_shadow_inputs(now).queued_frames)
        << "nothing placed, nothing draining";
}

TEST(PendingShadowInputs, OpsAndDeferredOpsAreVisible) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    const int64_t now = esp_timer_get_time();

    sendOpen(h);
    ASSERT_TRUE(h.rol.awaitingAck());
    EXPECT_TRUE(h.rol.pending_shadow_inputs(now).op_awaiting_ack);
    deliverAck(h.rol, P(h).op_first_msgid_);
    EXPECT_FALSE(h.rol.pending_shadow_inputs(now).op_awaiting_ack);

    P(h).relogin_pending_ = true;
    sendOpen(h);   // parked behind the relogin
    EXPECT_TRUE(P(h).op_deferred_until_login_);
    const pendingshadow::NodeInputs in = h.rol.pending_shadow_inputs(now);
    EXPECT_TRUE(in.op_deferred_until_login);
    EXPECT_TRUE(in.relogin_pending);
}

TEST(PendingShadowInputs, TrafficAgeComesFromHearingAndFromAddressingTheNode) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    proto_sim_timer_set_now_us(10'000'000);
    h.rol.note_node_heard_();
    proto_sim_timer_advance_us(400'000'000);   // ~6.7 min later
    pendingshadow::NodeInputs in = h.rol.pending_shadow_inputs(esp_timer_get_time());
    EXPECT_EQ(in.since_last_traffic_us, 400'000'000);

    // A downlink placed for it counts, once its mark has passed.
    P(h).last_placed_t0_us_ = esp_timer_get_time() - 5'000'000;
    in = h.rol.pending_shadow_inputs(esp_timer_get_time());
    EXPECT_EQ(in.since_last_traffic_us, 5'000'000);

    // ...and while its mark is still ahead it is a QUEUED frame.
    P(h).last_placed_t0_us_ = esp_timer_get_time() + 800'000;
    in = h.rol.pending_shadow_inputs(esp_timer_get_time());
    EXPECT_TRUE(in.queued_frames);

    // The hub's own air being busy also counts, for every node.
    P(h).last_placed_t0_us_ = 0;
    h.tracker.tx_drain_us = 1'000'000;
    EXPECT_TRUE(h.rol.pending_shadow_inputs(esp_timer_get_time()).queued_frames);
}
