// P0 coverage for the automatic (scheduled) node mode.
//
// Nothing in production SENDS these messages yet — P0 only adds the schema.
// What is worth locking down NOW is exactly the set of properties the rest of
// the plan depends on, because each one is cheap to break later and expensive
// to debug on a sleeping battery node:
//
//   1. Round-trip fidelity of the new messages through the REAL generated
//      stubs (the same blinds.pb-c.c the firmware links).
//   2. Forward compatibility: an OLD parser must treat a NEW message as a
//      quiet unknown, not an error.  This is what makes the node-first deploy
//      order safe (see docs/auto-mode-plan.md §6.1).
//   3. The frame budget (D3) and the airtime constraint behind D5 — asserted
//      on real encoded bytes rather than a paper estimate.

#include <gtest/gtest.h>

#include "sim/messages.h"
#include "sim/wire_codec.h"
#include "sim/crypto.h"

extern "C" {
#include "blinds.pb-c.h"
}

#include <cmath>
#include <iostream>
#include <iterator>
#include <vector>

// NOTE: no `using namespace proto_sim` — blinds.pb-c.h declares the same
// type names in the GLOBAL namespace, so a using-directive makes every
// shared name ambiguous. Alias instead (see wire_codec.h).
namespace ps = proto_sim;

namespace {

ps::LoraHeader make_header() {
    ps::LoraHeader h;
    h.destAddress   = 17;
    h.destSubnet    = 2;
    h.senderAddress = 0x01;
    h.msgId         = 42;
    return h;
}

// A full 8-entry schedule — the maximum the plan allows (D3).
ps::ScheduleConfig make_full_schedule() {
    ps::ScheduleConfig s;
    s.version              = 0xDEADBEEF;
    s.mode                 = ps::NodeMode::MODE_AUTO;
    s.interactiveTimeout_s = 1800;   // 30 min (D4)
    s.checkinInterval_s    = 21600;  // 6 h   (D4)
    s.beaconLead_s         = 30;     // I1
    s.postEventWindow_s    = 20;
    s.catchupWindow_s      = 1800;   // I3
    for (int i = 0; i < 8; i++) {
        ps::ScheduleEntry e;
        // Spread across the day and use worst-case (largest varint) values so
        // the size assertions below are an upper bound, not a lucky sample.
        e.minuteOfDay = 1439 - i;          // near-max minute-of-day
        e.dayMask     = 0x7F;              // all seven days
        e.action      = (i % 2) ? ps::SchedAction::SCHED_POSITION : ps::SchedAction::SCHED_CLOSE;
        e.positionPct = 100;
        e.kind        = 2;                 // sunset
        s.entries.push_back(e);
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Round-trip fidelity through the real stubs
// ---------------------------------------------------------------------------

TEST(AutoModeProto, TimeSyncRoundTrip) {
    ps::LoraClientOperationMessage m;
    m.header = make_header();
    m.cmd    = ps::LoraClientOperationMessage::Cmd::TimeSync;
    m.timesync.epoch     = 1787000000ULL;   // well past 2^31 — catches a 32-bit truncation
    m.timesync.utcOffset = 7200;            // CEST
    m.timesync.dstNext   = 1793491200ULL;

    auto bytes = ps::serialize_op(m);
    auto back  = ps::deserialize_op(bytes.data(), bytes.size());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->cmd, ps::LoraClientOperationMessage::Cmd::TimeSync);
    EXPECT_EQ(back->timesync.epoch,     1787000000ULL);
    EXPECT_EQ(back->timesync.utcOffset, 7200);
    EXPECT_EQ(back->timesync.dstNext,   1793491200ULL);
}

TEST(AutoModeProto, TimeSyncNegativeUtcOffsetSurvives) {
    // utcOffset is int32 (signed) — a westward zone must not wrap to a huge
    // positive. A uint32 field here would silently break the western half of
    // the world; this pins the signedness.
    ps::LoraClientOperationMessage m;
    m.header = make_header();
    m.cmd    = ps::LoraClientOperationMessage::Cmd::TimeSync;
    m.timesync.epoch     = 1787000000ULL;
    m.timesync.utcOffset = -18000;          // UTC-5

    auto bytes = ps::serialize_op(m);
    auto back  = ps::deserialize_op(bytes.data(), bytes.size());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->timesync.utcOffset, -18000);
}

TEST(AutoModeProto, ScheduleConfigRoundTripPreservesAllEntries) {
    ps::LoraClientOperationMessage m;
    m.header   = make_header();
    m.cmd      = ps::LoraClientOperationMessage::Cmd::Schedule;
    m.schedule = make_full_schedule();

    auto bytes = ps::serialize_op(m);
    auto back  = ps::deserialize_op(bytes.data(), bytes.size());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->cmd, ps::LoraClientOperationMessage::Cmd::Schedule);

    const auto& s = back->schedule;
    EXPECT_EQ(s.version,              0xDEADBEEFu);
    EXPECT_EQ(s.mode,                 ps::NodeMode::MODE_AUTO);
    EXPECT_EQ(s.interactiveTimeout_s, 1800u);
    EXPECT_EQ(s.checkinInterval_s,    21600u);
    EXPECT_EQ(s.beaconLead_s,         30u);
    EXPECT_EQ(s.postEventWindow_s,    20u);
    EXPECT_EQ(s.catchupWindow_s,      1800u);

    ASSERT_EQ(s.entries.size(), 8u);
    for (size_t i = 0; i < s.entries.size(); i++) {
        EXPECT_EQ(s.entries[i].minuteOfDay, 1439u - i) << "entry " << i;
        EXPECT_EQ(s.entries[i].dayMask,     0x7Fu)     << "entry " << i;
        EXPECT_EQ(s.entries[i].positionPct, 100u)      << "entry " << i;
        EXPECT_EQ(s.entries[i].kind,        2u)        << "entry " << i;
        EXPECT_EQ(s.entries[i].action,
                  (i % 2) ? ps::SchedAction::SCHED_POSITION : ps::SchedAction::SCHED_CLOSE)
            << "entry " << i;
    }
}

TEST(AutoModeProto, EmptyScheduleRoundTrips) {
    // Q9: a node with no entries must stay INTERACTIVE. An empty repeated field
    // has to survive the round trip as genuinely empty (not as one zero entry).
    ps::LoraClientOperationMessage m;
    m.header               = make_header();
    m.cmd                  = ps::LoraClientOperationMessage::Cmd::Schedule;
    m.schedule.version     = 1;
    m.schedule.mode        = ps::NodeMode::MODE_INTERACTIVE;

    auto bytes = ps::serialize_op(m);
    auto back  = ps::deserialize_op(bytes.data(), bytes.size());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->cmd, ps::LoraClientOperationMessage::Cmd::Schedule);
    EXPECT_TRUE(back->schedule.entries.empty());
    EXPECT_EQ(back->schedule.mode, ps::NodeMode::MODE_INTERACTIVE);
}

TEST(AutoModeProto, WakeBeaconRoundTrip) {
    ps::LoraClientResponseMessage m;
    m.header = make_header();
    m.header.senderAddress = 17;
    m.proto  = ps::LoraClientResponseMessage::Proto::Beacon;
    m.beacon.reason         = ps::WakeReason::WAKE_TIMER_EVENT;
    m.beacon.schedVersion   = 0xDEADBEEF;
    m.beacon.nodeEpoch      = 1787000123ULL;
    m.beacon.mode           = ps::NodeMode::MODE_AUTO;
    m.beacon.voltage        = 3.97f;
    m.beacon.position       = 0.42f;
    m.beacon.awakeWindow_ms = 20000;
    m.beacon.nextEventEpoch = 1787025600ULL;
    m.beacon.sessionResume  = true;
    m.beacon.clockValid     = true;
    m.beacon.fwVersion      = 10013;

    auto bytes = ps::serialize_resp(m);
    auto back  = ps::deserialize_resp(bytes.data(), bytes.size());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->proto, ps::LoraClientResponseMessage::Proto::Beacon);
    EXPECT_EQ(back->beacon.reason,         ps::WakeReason::WAKE_TIMER_EVENT);
    EXPECT_EQ(back->beacon.schedVersion,   0xDEADBEEFu);
    EXPECT_EQ(back->beacon.nodeEpoch,      1787000123ULL);
    EXPECT_EQ(back->beacon.mode,           ps::NodeMode::MODE_AUTO);
    EXPECT_FLOAT_EQ(back->beacon.voltage,  3.97f);
    EXPECT_FLOAT_EQ(back->beacon.position, 0.42f);
    EXPECT_EQ(back->beacon.awakeWindow_ms, 20000u);
    EXPECT_EQ(back->beacon.nextEventEpoch, 1787025600ULL);
    EXPECT_TRUE(back->beacon.sessionResume);
    EXPECT_TRUE(back->beacon.clockValid);
    EXPECT_EQ(back->beacon.fwVersion,      10013u);
}

TEST(AutoModeProto, ModeSysopsRoundTrip) {
    for (auto op : {ps::ClientOperation::CMD_MODE_AUTO, ps::ClientOperation::CMD_MODE_INTERACTIVE}) {
        ps::LoraClientOperationMessage m;
        m.header = make_header();
        m.cmd    = ps::LoraClientOperationMessage::Cmd::Sysop;
        m.sysop  = op;

        auto bytes = ps::serialize_op(m);
        auto back  = ps::deserialize_op(bytes.data(), bytes.size());
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->cmd,   ps::LoraClientOperationMessage::Cmd::Sysop);
        EXPECT_EQ(back->sysop, op);
    }
}

// ---------------------------------------------------------------------------
// 2. Forward compatibility — the gate that makes node-first deployment safe
// ---------------------------------------------------------------------------

TEST(AutoModeProto, UnknownOneofFieldDecodesAsNotSetNotError) {
    // Simulates an OLD peer receiving a NEW message: we encode a real
    // ps::ScheduleConfig, then decode it as a RESPONSE message, whose oneof has no
    // field 17 at all. protobuf-c must skip it as an unknown field and leave
    // proto_case unset — NOT return nullptr (which production code logs as
    // "Could not read protobuf" and would flood on every beacon).
    ps::LoraClientOperationMessage m;
    m.header   = make_header();
    m.cmd      = ps::LoraClientOperationMessage::Cmd::Schedule;
    m.schedule = make_full_schedule();
    auto bytes = ps::serialize_op(m);

    ::LoraClientResponseMessage* pb =
        lora_client_response_message__unpack(nullptr, bytes.size(), bytes.data());
    ASSERT_NE(pb, nullptr) << "unknown field must not fail the parse";
    EXPECT_EQ(pb->proto_case, LORA_CLIENT_RESPONSE_MESSAGE__PROTO__NOT_SET);
    // The header is a known field and must still decode — this is what lets an
    // old hub run its msgid replay check on a message it cannot interpret.
    ASSERT_NE(pb->header, nullptr);
    EXPECT_EQ(pb->header->destaddress, 17u);
    EXPECT_EQ(pb->header->msgid,       42u);
    lora_client_response_message__free_unpacked(pb, nullptr);
}

TEST(AutoModeProto, FutureFieldNumberDecodesAsNotSetKeepingHeaderReadable) {
    // The concrete node-first case: a node running NEWER firmware sends a
    // message whose oneof field number the OLD hub's stubs do not contain at
    // all.  Hand-craft that rather than reusing an existing message, because
    // every field number in this schema is deliberately reused across the two
    // message types (field 10 is `operation` downlink and `avail` uplink, and
    // so on) — direction picks the parser, so cross-parsing a real message
    // would test a case that cannot occur on air.
    //
    // Frame = field 1 (header, submessage) + field 20 (unknown, length-delim).
    ps::LoraClientResponseMessage hdr_only;
    hdr_only.header = make_header();
    hdr_only.header.senderAddress = 17;
    hdr_only.proto  = ps::LoraClientResponseMessage::Proto::NotSet;
    std::vector<uint8_t> bytes = ps::serialize_resp(hdr_only);

    // Append field 20, wire type 2 (length-delimited): tag = 20<<3 | 2 = 162,
    // which is a two-byte varint (0xA2 0x01).
    const uint8_t unknown_payload[] = {0x08, 0x2A};   // arbitrary inner bytes
    bytes.push_back(0xA2);
    bytes.push_back(0x01);
    bytes.push_back(sizeof(unknown_payload));
    bytes.insert(bytes.end(), std::begin(unknown_payload), std::end(unknown_payload));

    ::LoraClientResponseMessage* pb =
        lora_client_response_message__unpack(nullptr, bytes.size(), bytes.data());
    ASSERT_NE(pb, nullptr) << "an unknown field number must not fail the parse";
    EXPECT_EQ(pb->proto_case, LORA_CLIENT_RESPONSE_MESSAGE__PROTO__NOT_SET);
    // The header must still be readable — this is what lets an old hub run its
    // msgid replay check and login_acked_ bookkeeping on a message whose
    // payload it cannot interpret.
    ASSERT_NE(pb->header, nullptr);
    EXPECT_EQ(pb->header->senderaddress, 17u);
    EXPECT_EQ(pb->header->msgid,         42u);
    lora_client_response_message__free_unpacked(pb, nullptr);
}

TEST(AutoModeProto, ExistingMessagesUnaffectedByNewFields) {
    // Regression guard for the P0 "no behaviour change" claim: a plain cover
    // operation must still encode to exactly what it did before, and decode
    // with the same case. If someone renumbers a field later, this fails.
    ps::LoraClientOperationMessage m;
    m.header = make_header();
    m.cmd    = ps::LoraClientOperationMessage::Cmd::Operation;
    m.operation.kind      = ps::LoraCoverOperation::Kind::Operation;
    m.operation.operation = ps::CovOperation::CMD_OPEN;

    auto bytes = ps::serialize_op(m);
    auto back  = ps::deserialize_op(bytes.data(), bytes.size());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->cmd, ps::LoraClientOperationMessage::Cmd::Operation);
    EXPECT_EQ(back->operation.operation, ps::CovOperation::CMD_OPEN);
    EXPECT_EQ(back->header.msgId, 42u);
}

// ---------------------------------------------------------------------------
// 3. Frame budget (D3) and airtime constraint (D5)
// ---------------------------------------------------------------------------

namespace {

// LoRa time-on-air, Semtech SX1276/78 datasheet §4.1.1.7.
// Explicit header, CRC on, preamble 8 — matching the radio params both sides
// compile in (SF7, BW500, CR4/8).
double time_on_air_ms(size_t payload_bytes, int sf = 7, double bw_hz = 500000.0,
                      int cr_denom = 8, int preamble = 8) {
    const double t_sym_ms = (double)(1 << sf) / bw_hz * 1000.0;
    const int    de       = 0;               // low-data-rate optimise off at BW500/SF7
    const int    header   = 0;               // explicit header
    const int    cr       = cr_denom - 4;    // 4/8 -> 4
    double num = 8.0 * (double)payload_bytes - 4.0 * sf + 28.0 + 16.0 - 20.0 * header;
    double den = 4.0 * (sf - 2 * de);
    double n_payload = 8.0 + std::fmax(std::ceil(num / den) * (cr + 4), 0.0);
    double t_preamble_ms = (preamble + 4.25) * t_sym_ms;
    return t_preamble_ms + n_payload * t_sym_ms;
}

// Encrypted frames replace the plaintext payload with an AEAD envelope:
// 16-byte tag + ciphertext the same length as the inner payload, plus protobuf
// framing for both. This models that overhead so the budget covers the real
// on-air case rather than the plaintext one.
constexpr size_t kAeadOverheadBytes = ps::kOnAirTagBytes + 2 /*tag tag+len*/
                                    + 3 /*ciphertext tag+len*/;

} // namespace

TEST(AutoModeFrameBudget, FullScheduleFitsInOneLoRaFrame) {
    // D3: the SX1278 payload limit is 255 bytes (REG_PAYLOAD_LENGTH is 8-bit,
    // and both drivers set FIFO_{TX,RX}_BASE_ADDR = 0 so the whole 256-byte
    // FIFO backs a single packet — no 128/128 split).
    constexpr size_t kMaxFrame = 255;

    ps::LoraClientOperationMessage m;
    m.header   = make_header();
    m.cmd      = ps::LoraClientOperationMessage::Cmd::Schedule;
    m.schedule = make_full_schedule();

    const size_t plain = ps::serialize_op(m).size();
    const size_t onair = plain + kAeadOverheadBytes;

    // Printed so the real numbers are visible in test output rather than
    // living only in a design doc that can drift.
    std::cout << "[budget] 8-entry ScheduleConfig: plaintext " << plain
              << " B, on-air (AEAD) " << onair << " B, airtime "
              << time_on_air_ms(onair) << " ms (limit 255 B)\n";

    EXPECT_LE(onair, kMaxFrame)
        << "8-entry ps::ScheduleConfig no longer fits in one frame ("
        << onair << " B). Either trim fields or switch to the bit-packed "
        << "repeated-uint32 form described in docs/auto-mode-plan.md §3.3.";

    // Keep meaningful headroom so a later field addition does not silently
    // creep up to the cliff.
    EXPECT_LE(onair, 200u) << "on-air size " << onair
                           << " B is close to the 255 B limit — headroom is gone";
}

TEST(AutoModeFrameBudget, ScheduleAirtimeExceedsBurstSlotHenceD5) {
    // D5's justification, asserted rather than assumed: the hub bursts every
    // downlink txSlotsPerRound=17 times at roundDurationMs/txSlotsPerRound =
    // 1500/17 = 88 ms. A full ps::ScheduleConfig does NOT fit in that slot, which
    // is why it must be sent single-shot (burstCount = 0).
    constexpr double kBurstSlotMs = 1500.0 / 17.0;   // 88.2 ms

    ps::LoraClientOperationMessage m;
    m.header   = make_header();
    m.cmd      = ps::LoraClientOperationMessage::Cmd::Schedule;
    m.schedule = make_full_schedule();
    const size_t onair = ps::serialize_op(m).size() + kAeadOverheadBytes;

    EXPECT_GT(time_on_air_ms(onair), kBurstSlotMs)
        << "A full ps::ScheduleConfig now fits inside one burst slot. If that is "
        << "genuinely true, D5 (single-shot send) can be revisited — but "
        << "verify against the node's hard-coded kBurstTxIntervalMs = 88 first.";
}

TEST(AutoModeFrameBudget, TypicalTrafficStillFitsBurstSlot) {
    // The flip side: today's frames must stay comfortably inside the slot, or
    // the existing burst machinery is already mistimed. Guards against someone
    // fattening a common message without noticing.
    constexpr double kBurstSlotMs = 1500.0 / 17.0;

    ps::LoraClientOperationMessage m;
    m.header = make_header();
    m.cmd    = ps::LoraClientOperationMessage::Cmd::Operation;
    m.operation.kind      = ps::LoraCoverOperation::Kind::Operation;
    m.operation.operation = ps::CovOperation::CMD_CLOSE;
    const size_t onair = ps::serialize_op(m).size() + kAeadOverheadBytes;

    EXPECT_LT(time_on_air_ms(onair), kBurstSlotMs * 0.75)
        << "A routine cover op (" << onair << " B, "
        << time_on_air_ms(onair) << " ms) is crowding the 88 ms burst slot.";
}

TEST(AutoModeFrameBudget, BeaconIsSmall) {
    // The beacon is sent on every wake, so its airtime is a direct battery
    // cost. Keep it well under a burst slot.
    ps::LoraClientResponseMessage m;
    m.header = make_header();
    m.proto  = ps::LoraClientResponseMessage::Proto::Beacon;
    m.beacon.reason         = ps::WakeReason::WAKE_TIMER_EVENT;
    m.beacon.schedVersion   = 0xDEADBEEF;
    m.beacon.nodeEpoch      = 1787000123ULL;
    m.beacon.mode           = ps::NodeMode::MODE_AUTO;
    m.beacon.voltage        = 3.97f;
    m.beacon.position       = 0.42f;
    m.beacon.awakeWindow_ms = 20000;
    m.beacon.nextEventEpoch = 1787025600ULL;
    m.beacon.sessionResume  = true;
    m.beacon.clockValid     = true;
    m.beacon.fwVersion      = 10013;

    const size_t onair = ps::serialize_resp(m).size() + kAeadOverheadBytes;
    EXPECT_LT(onair, 120u) << "beacon grew to " << onair << " B";
}
