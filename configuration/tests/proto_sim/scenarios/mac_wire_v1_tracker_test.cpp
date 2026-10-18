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
