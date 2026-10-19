// S8 / W3: the management unit's v1 beacon (AUTH = FLEET) against the shared
// vector, and its refusals. Runs in both configurations (packBeaconV1 is
// compiled either way; buildBeacon uses it under MAC_WIRE_V1).

#include "mac_mgmt.h"
#include "lora_tracker.h"

#include "blinds.pb-c.h"
#include "MacWire.h"
#include "MacWireVectors.h"

#include <psa/crypto.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace mv = macwire_vectors;

namespace {
const mv::Vector &beaconVector() {
    for (size_t i = 0; i < mv::kVectorCount; ++i)
        if (std::strcmp(mv::kVectors[i].name, "grid_beacon") == 0)
            return mv::kVectors[i];
    ADD_FAILURE() << "no grid_beacon vector";
    return mv::kVectors[0];
}
}  // namespace

TEST(MacWireV1Beacon, TheFleetTaggedBeaconIsTheVectorsBytes) {
    ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS);
    const mv::Vector &v = beaconVector();
    macmgmt::HubMacManagement mgmt;
    std::memcpy(mgmt.netKeyForTest(), mv::kKFleet, 16);
    mgmt.setNetKeyIdForTest(mv::kFleetKeyId);

    HubMacPdu *pdu = hub_mac_pdu__unpack(nullptr, v.body_len, v.body);
    ASSERT_NE(pdu, nullptr);
    ASSERT_EQ(pdu->pdu_case, HUB_MAC_PDU__PDU_GRIDBEACON);
    std::vector<uint8_t> out;
    ASSERT_EQ(mgmt.packBeaconV1(pdu->gridbeacon, 255, &out), macmgmt::BeaconBuild::Ok);
    hub_mac_pdu__free_unpacked(pdu, nullptr);
    EXPECT_EQ(out, std::vector<uint8_t>(v.frame, v.frame + v.frame_len));
}

TEST(MacWireV1Beacon, NoKeyNoBeaconAndTooBigIsRefused) {
    ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS);
    macmgmt::HubMacManagement mgmt;   // never minted: no key
    GridBeacon gb = GRID_BEACON__INIT;
    gb.txround = 7;
    std::vector<uint8_t> out;
    EXPECT_EQ(mgmt.packBeaconV1(&gb, 255, &out), macmgmt::BeaconBuild::Unsigned);
    EXPECT_TRUE(out.empty());
    mgmt.mintFleetKey();
    EXPECT_EQ(mgmt.packBeaconV1(&gb, 20, &out), macmgmt::BeaconBuild::TooBig);
    ASSERT_EQ(mgmt.packBeaconV1(&gb, 255, &out), macmgmt::BeaconBuild::Ok);
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    ASSERT_TRUE(macwire::decode(out.data(), out.size(), &h, &off, &blen));
    EXPECT_EQ(h.auth(), macwire::Auth::Fleet);
    EXPECT_EQ(h.dst, macwire::kBroadcast);
    EXPECT_EQ(h.fcnt, 0u);
    EXPECT_EQ(h.burst_cnt, 1);
    EXPECT_FALSE(h.stamped()) << "a beacon is never stamped (kBeaconPayloadBytes)";
    uint8_t tag[8];
    ASSERT_TRUE(mgmt.fleetTagV1(out.data(), out.size() - 8, tag));
    EXPECT_EQ(0, std::memcmp(tag, out.data() + out.size() - 8, 8));
}

// ---------------------------------------------------------------------------
// StackWatch (both configurations)
// ---------------------------------------------------------------------------
#include "StackWatch.h"

TEST(StackWatch, FirstAtBootPlusAMinuteThenEveryFiveMinutes) {
    int64_t last = 0;
    EXPECT_FALSE(stackwatch::due(stackwatch::kFirstAfterUs - 1, &last));
    EXPECT_TRUE(stackwatch::due(stackwatch::kFirstAfterUs, &last));
    EXPECT_EQ(last, stackwatch::kFirstAfterUs);
    EXPECT_FALSE(stackwatch::due(stackwatch::kFirstAfterUs + stackwatch::kEveryUs - 1, &last));
    EXPECT_TRUE(stackwatch::due(stackwatch::kFirstAfterUs + stackwatch::kEveryUs, &last));
    EXPECT_EQ(stackwatch::kFirstAfterUs, 60LL * 1000000);
    EXPECT_EQ(stackwatch::kEveryUs, 300LL * 1000000);
}

// The H-1 max now says which sample it was (uptime, copy index).
namespace {
struct H1Probe : esphome::lora_tracker::LORATracker {
    using LORATracker::noteTagWriteLatencyUs_;
    using LORATracker::tag_latency_report_ready_;
    using LORATracker::tag_latency_report_max_us_;
    using LORATracker::tag_latency_report_max_at_us_;
    using LORATracker::tag_latency_report_max_copy_;
};
}  // namespace

TEST(H1Histogram, TheMaxSampleCarriesItsInstantAndCopy) {
    H1Probe t;
    for (int i = 0; i < 1999; ++i)
        t.noteTagWriteLatencyUs_(i == 700 ? 13474u : 1500u, 1000000LL + i, i == 700 ? 4 : 0);
    EXPECT_FALSE(t.tag_latency_report_ready_);
    t.noteTagWriteLatencyUs_(1500u, 9999999LL, 1);
    ASSERT_TRUE(t.tag_latency_report_ready_);
    EXPECT_EQ(t.tag_latency_report_max_us_, 13474u);
    EXPECT_EQ(t.tag_latency_report_max_at_us_, 1000700LL);
    EXPECT_EQ(t.tag_latency_report_max_copy_, 4);
}

#if defined(MAC_WIRE_V1) && MAC_WIRE_V1
// ---------------------------------------------------------------------------
// The REAL burst loop over v1 frames (spec section 5): per copy only byte
// writes and one sealer call over the copy's raw bytes; never a decode.
// ---------------------------------------------------------------------------
#include "lora_hal_recorder.h"

using esphome::lora_tracker::LORATracker;

namespace {
struct SealLog {
    int calls{0};
    bool answer{true};
    uint64_t binding{0};
    std::vector<std::vector<uint8_t>> inputs;   // what the sealer was asked to tag
    std::vector<mac::MacHdr> hdrs;
};
SealLog g_log;
bool fakeSealer(void *ctx, const mac::MacHdr &hdr, const uint8_t *bytes, size_t n, uint8_t tag[8]) {
    g_log.binding = *static_cast<uint64_t *>(ctx);
    g_log.hdrs.push_back(hdr);
    g_log.inputs.emplace_back(bytes, bytes + n);
    for (int i = 0; i < 8; ++i) tag[i] = (uint8_t) (0xC0 + g_log.calls);
    ++g_log.calls;
    return g_log.answer;
}
std::vector<uint8_t> v1Frame(macwire::Auth a) {
    macwire::WireHdr h;
    h.flags = macwire::withAuth(macwire::kFlagAckReq, a);
    h.dst = 0x12; h.src = 1; h.subnet = 2; h.fcnt = 42; h.port = macwire::kPortApp;
    const size_t hlen = macwire::headerLen(h);
    std::vector<uint8_t> f(hlen + 5 + macwire::tagLen(h.flags), 0);
    macwire::encodeHeader(h, f.data(), f.size());
    for (int i = 0; i < 5; ++i) f[hlen + i] = (uint8_t) (0x50 + i);   // "ciphertext"
    return f;
}
constexpr uint64_t kBinding = 0x8000001200000005ULL;
}  // namespace

TEST(MacWireV1Burst, EveryCopyIsStampedIndexedAndTaggedOverItsOwnBytes) {
    lorahal::rec().reset();
    proto_sim_timer_reset();
    proto_sim_timer_set_now_us(3'000'000);
    g_log = SealLog{};
    LORATracker t;
    t.startGrid();
    auto f = v1Frame(macwire::Auth::Session);
    t.sendPacketBurst(f.data(), f.size(), /*copies=*/3, /*stride_ms=*/40, 0, false,
                      &fakeSealer, kBinding);
    ASSERT_EQ(lorahal::rec().packets.size(), 3u);
    ASSERT_EQ(g_log.calls, 3);
    EXPECT_EQ(g_log.binding, kBinding);
    for (size_t k = 0; k < 3; ++k) {
        const auto &p = lorahal::rec().packets[k];
        ASSERT_EQ(p.size(), f.size() + macwire::kStampBytes) << "the stamp was inserted";
        macwire::WireHdr h;
        size_t off = 0, blen = 0;
        ASSERT_TRUE(macwire::decode(p.data(), p.size(), &h, &off, &blen));
        EXPECT_EQ(h.burst_idx, k);
        EXPECT_EQ(h.burst_cnt, 3);
        EXPECT_TRUE(h.stamped());
        EXPECT_EQ(g_log.hdrs[k].burst_idx, k) << "the sealer's MacHdr is the stamped header";
        EXPECT_EQ(g_log.inputs[k], std::vector<uint8_t>(p.begin(), p.end() - 8))
            << "the tag covers every transmitted byte before it";
        for (int i = 0; i < 8; ++i) EXPECT_EQ(p[p.size() - 8 + i], (uint8_t) (0xC0 + k));
        EXPECT_EQ(std::vector<uint8_t>(p.begin() + off, p.begin() + off + 5),
                  std::vector<uint8_t>(f.begin() + 12, f.begin() + 17)) << "ciphertext untouched";
    }
}

TEST(MacWireV1Burst, ARefusedCopyIsDroppedAndATaglessFrameNeverSealed) {
    lorahal::rec().reset();
    g_log = SealLog{};
    g_log.answer = false;
    LORATracker t;
    auto f = v1Frame(macwire::Auth::Session);
    t.sendPacketBurst(f.data(), f.size(), 2, 0, 0, false, &fakeSealer, kBinding);
    EXPECT_EQ(lorahal::rec().packets.size(), 0u);

    lorahal::rec().reset();
    t.sendPacketBurst(f.data(), f.size(), 2, 0);   // no sealer at all
    EXPECT_EQ(lorahal::rec().packets.size(), 0u) << "never sent half-sealed";

    lorahal::rec().reset();
    g_log = SealLog{};
    auto plain = v1Frame(macwire::Auth::None);
    t.sendPacketBurst(plain.data(), plain.size(), 2, 0, 0, false, &fakeSealer, kBinding);
    EXPECT_EQ(g_log.calls, 0);
    ASSERT_EQ(lorahal::rec().packets.size(), 2u);
    EXPECT_EQ(lorahal::rec().packets[1][macwire::kOffBurstIdx], 1);
}

TEST(MacWireV1Burst, ASealedCopyLeavesAtItsStampedInstantWithItsTagAlreadyWritten) {
    // 2026-10-19 bench: Mode B + crypto phaseErr p50 1.6 ms against 0.19 ms
    // without crypto. The question: does the per-copy CMAC (~1.6 ms on the
    // ESP32) push the copy past the instant stamped into it? The hub's order is
    // stamp -> tag -> prepare -> busy-wait to the stamp -> fire, with the copy
    // released kPrepareLeadUs early, so the tag must be finished and the fire
    // must land ON the stamp. Here the sealer costs 1.6 ms of clock.
    lorahal::rec().reset();
    proto_sim_timer_reset();
    proto_sim_timer_set_now_us(3'000'000);
    g_log = SealLog{};
    LORATracker t;
    t.startGrid();
    const int64_t mark_t0 = t.nextT0ForSlotUs(18, 4'000'000);
    const int64_t fire    = loratiming::fireInstantUs(mark_t0, 0);
    proto_sim_timer_set_now_us(fire - LORATracker::kPrepareLeadUs);   // as popDue releases it
    struct Slow {
        static bool sealer(void *ctx, const mac::MacHdr &h, const uint8_t *b, size_t n, uint8_t tag[8]) {
            proto_sim_timer_advance_us(1600);
            g_log.binding = esp_timer_get_time();   // when the tag was finished
            return fakeSealer(ctx, h, b, n, tag);
        }
    };
    auto f = v1Frame(macwire::Auth::Session);
    t.sendPacketBurst(f.data(), f.size(), 1, 0, fire, /*on_mark=*/true, &Slow::sealer, kBinding);
    ASSERT_EQ(lorahal::rec().packets.size(), 1u);
    const auto &p = lorahal::rec().packets[0];
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    ASSERT_TRUE(macwire::decode(p.data(), p.size(), &h, &off, &blen));
    ASSERT_TRUE(h.stamped());
    const int64_t stamped_t0 = t.gridAnchorUs() + (int64_t) h.fire_round * (int64_t) timedgrid::kRoundUs +
                               (int64_t) h.fire_offset_us;
    EXPECT_EQ(stamped_t0, mark_t0) << "the stamp declares the mark";
    EXPECT_EQ(lorahal::rec().tx_us[0], fire) << "the copy fires ON its stamped instant";
    EXPECT_LT((int64_t) g_log.binding, fire) << "and its tag was complete before it";
    EXPECT_EQ(t.stampMisses(), 0u);
}

TEST(MacWireV1Burst, TheNodesT0FromTheFinalOnAirLengthIsTheStampedT0) {
    // The node's T0 is SFD end: rxdone - t0ToRxDoneUs(on-air length). The hub
    // fires at T0 - preamble (fireInstantUs), which does not depend on the
    // length at all; the length only has to be the FINAL one at the node. So
    // for every layout (AUTH NONE / SESSION, stamped or not, several bodies),
    // a node receiving the copy actually sent derives exactly the stamped T0.
    for (macwire::Auth a : {macwire::Auth::None, macwire::Auth::Session}) {
        for (size_t body : {0u, 5u, 40u, 150u}) {
            for (bool grid : {true, false}) {
                lorahal::rec().reset();
                proto_sim_timer_reset();
                proto_sim_timer_set_now_us(3'000'000);
                g_log = SealLog{};
                LORATracker t;
                if (grid) t.startGrid();
                const int64_t mark_t0 = grid ? t.nextT0ForSlotUs(18, 4'000'000) : 4'000'000;
                const int64_t fire    = loratiming::fireInstantUs(mark_t0, 0);
                proto_sim_timer_set_now_us(fire - LORATracker::kPrepareLeadUs);
                macwire::WireHdr h;
                h.flags = macwire::withAuth(0, a);
                h.dst = 0x12; h.src = 1; h.subnet = 2; h.fcnt = 7; h.port = macwire::kPortApp;
                std::vector<uint8_t> f(macwire::headerLen(h) + body + macwire::tagLen(h.flags), 0x5A);
                macwire::encodeHeader(h, f.data(), f.size());
                t.sendPacketBurst(f.data(), f.size(), 1, 0, fire, true,
                                  a == macwire::Auth::Session ? &fakeSealer : nullptr, kBinding);
                ASSERT_EQ(lorahal::rec().packets.size(), 1u);
                const auto &p = lorahal::rec().packets[0];
                const int64_t tx_start = lorahal::rec().tx_us[0];
                const int64_t rxdone   = tx_start + (int64_t) loratiming::kDownlinkPreambleToT0Us +
                                         (int64_t) loratiming::t0ToRxDoneUs((uint32_t) p.size());
                const int64_t node_t0  = loratiming::t0FromRxDoneUs(rxdone, (uint32_t) p.size());
                EXPECT_EQ(node_t0, mark_t0) << "auth " << (int) a << " body " << body << " grid " << grid;
                EXPECT_EQ(p.size(), f.size() + (grid ? macwire::kStampBytes : 0u))
                    << "the stamp is part of the on-air length";
            }
        }
    }
}

namespace {
struct PoolProbe : LORATracker {
    void init() { this->init_memory_pool(); }
};
}  // namespace

TEST(MacWireV1Burst, APlacedFramesReservationIsPricedWithTheStampItWillCarry) {
    // The frame is packed unstamped; sendPacketBurst inserts the 6-byte STAMP.
    // Its reservation must end where the STAMPED copy's airtime ends.
    proto_sim_timer_reset();
    proto_sim_timer_set_now_us(3'000'000);
    PoolProbe t;
    t.init();
    t.startGrid();
    const int64_t fire = loratiming::fireInstantUs(t.nextT0ForSlotUs(18, 4'000'000), 0);
    // 40 B of body: 52 B packed -> 58 B on the air, across a symbol block.
    macwire::WireHdr h;
    h.flags = macwire::withAuth(0, macwire::Auth::None);
    h.dst = 0x12; h.src = 1; h.subnet = 2; h.fcnt = 7; h.port = macwire::kPortApp;
    for (size_t body : {40u, 41u, 42u, 43u, 44u}) {
        std::vector<uint8_t> f(macwire::headerLen(h) + body, 0x5A);
        macwire::encodeHeader(h, f.data(), f.size());
        esphome::lora_tracker::TxPolicy p;
        p.copies = 1;
        p.earliest_us = fire;
        p.expects_reply = false;
        ASSERT_TRUE(t.send(f.data(), f.size(), p));
        const int64_t want = fire + (int64_t) loratiming::kDownlinkPreambleToT0Us +
                             (int64_t) loratiming::t0ToRxDoneUs((uint32_t) (f.size() + macwire::kStampBytes));
        EXPECT_EQ(t.placedBusyUntilUs(), want) << "body " << body;
    }
}

TEST(MacWireV1Burst, TheFleetBeaconGoesOutVerbatim) {
    lorahal::rec().reset();
    const mv::Vector &v = beaconVector();
    LORATracker t;
    t.startGrid();
    std::vector<uint8_t> f(v.frame, v.frame + v.frame_len);
    t.sendPacketBurst(f.data(), f.size(), 1, 0, 0, false, nullptr, 0, /*fire_stamp=*/false);
    ASSERT_EQ(lorahal::rec().packets.size(), 1u);
    EXPECT_EQ(lorahal::rec().packets[0], f);
}
#endif   // MAC_WIRE_V1
