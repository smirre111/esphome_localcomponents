#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// LoraTiming — the single definition of LoRa frame timing, shared by the hub,
// the node and the host tests.
//
// EVERY instant in the timed-window design is relative to T0, the end of the
// start-frame delimiter. T0 is the only event both ends can name from their own
// hardware without agreeing on anything first:
//
//   air_start                T0 = SFD end        (ValidHeader)         RxDone
//       |                          |                  |                   |
//       |<-- n_pre up-chirps --><sync><SFD>|<- header 8 sym ->|<- payload ->|
//       |<------ k{Down,Up}linkPreambleToT0Us ->|
//
//   hub:  T0 = t_fire + d_tx_ramp + kDownlinkPreambleToT0Us
//   node: T0 = t_rxdone - symbolCount(len) * kSymbolUs        <- ONE LINE
//
// That last line is deliberately not a decomposition. Subtracting the payload
// time and the header time separately is arithmetically identical and invites a
// 2048 us double-count; an earlier draft of the design made exactly that error,
// which is why t0FromRxDoneUs() exists and callers must not open-code it.
//
// Dependency-free (stdint/stddef only), like BootPolicy.h and FrameCrypto.h, so
// the host harness verifies these numbers directly rather than on a roof.
//
// See configuration/docs/implementation-plan.md section 2.1-2.2 and
// configuration/docs/test-plan.md section 5.1.
// ---------------------------------------------------------------------------

namespace loratiming
{

// --- The radio configuration of this link: THE ONE DEFINITION -------------
// Every LoRa parameter the hub and the node program into their SX127x is
// defined here and nowhere else. LoraInterface (node) and LORATracker (hub) set
// their radios up from these values, and all the timing below is derived from
// the same numbers, so the radio and the arithmetic cannot disagree. The hub
// builds against a vendored copy of this file (proto/regen_stubs.sh).
static constexpr uint32_t kFrequencyHz       = 433300000;   // 433.05 MHz + 250 kHz
static constexpr uint8_t  kSyncWord          = 0x12;
static constexpr uint8_t  kSpreadingFactor   = 7;
static constexpr uint32_t kBandwidthHz       = 500000;
static constexpr uint8_t  kCodingRateDenom   = 8;   // CR 4/8 -> the "+4" term is 4
static constexpr bool     kCrcOn             = true;
static constexpr bool     kImplicitHeader    = false;
static constexpr bool     kLowDataRateOpt    = false;

// The preamble, per direction.
//
// DOWNLINK (hub -> node) is longer. Measured 2026-09-15 on node 2 (fw 1.0.83):
// strong, on-time Mode B frames (-45/-48 dBm) still failed to synchronise on an
// 8-symbol preamble, 2 of 597, and a window restarted after the failure had no
// preamble left to detect. 12 symbols cost 1 024 us of airtime per downlink.
// UPLINK (node -> hub) stays at 8: the hub listens continuously.
//
// A receiver is programmed with the longest preamble it expects (SX127x
// datasheet), so the node listens for the downlink length and the hub for the
// uplink length.
static constexpr uint8_t  kDownlinkPreambleSymbols = 12;
static constexpr uint8_t  kUplinkPreambleSymbols   = 8;

// T_sym = 2^SF / BW. At SF7/BW500 this is exactly 256 us; the expression is
// kept general so a bandwidth change is a compile-time result, not a hand edit.
static constexpr uint32_t kSymbolUs =
    (uint32_t) (((uint64_t) 1u << kSpreadingFactor) * 1000000ull / kBandwidthHz);

// air start -> T0. The preamble is n_pre up-chirps plus 4.25 symbols of sync
// word and SFD. Expressed in quarters to stay in integer arithmetic.
constexpr uint32_t preambleToT0Us(uint8_t preamble_symbols)
{
    return ((uint32_t) preamble_symbols * 4u + 17u) * kSymbolUs / 4u;
}
static constexpr uint32_t kDownlinkPreambleToT0Us = preambleToT0Us(kDownlinkPreambleSymbols);
static constexpr uint32_t kUplinkPreambleToT0Us   = preambleToT0Us(kUplinkPreambleSymbols);

// T0 -> ValidHeader. Structural only on this hardware: DIO3 is not routed to
// the CPU on the node PCB, so ValidHeader cannot be observed. Kept because the
// frame anatomy is easier to check against the datasheet with it present.
static constexpr uint32_t kHeaderSymbols = 8;
static constexpr uint32_t kHeaderUs      = kHeaderSymbols * kSymbolUs;

// The hub's burst copy spacing. EXACTLY 88 000 us.
//
// DriftEstimator.h records what the alternative costs: reading the 17 copies of
// a 1500 ms round as 1500/17 = 88.235 ms rather than the hub's integer 88.000
// produced a -2663 ppm error that survived three firmware revisions. A
// microsecond-resolution timer invites the same mistake, so the number lives
// here once and both ends take it from here.
static constexpr uint32_t kBurstCopyStrideUs = 88000;
static constexpr uint8_t  kBurstCopies       = 17;

// ---------------------------------------------------------------------------
// Symbol count for a payload.
//
//   n_sym = 8 + max(ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH)
//                        / (4*(SF - 2*DE))) * (CR + 4), 0)
//
// PL is the PAYLOAD LENGTH AS THE RADIO COUNTS IT — RegRxNbBytes — which
// EXCLUDES the CRC; the +16*CRC term accounts for the CRC separately. Getting
// that convention wrong is worth 0 OR 2048 us discontinuously, depending on
// where the error falls relative to a block boundary, because the ceil steps in
// blocks of (CR+4) symbols per 4*(SF-2*DE) bits. In the field it would present
// as an intermittent crystal fault. See test-plan.md section 5.1.
// ---------------------------------------------------------------------------
constexpr uint32_t symbolCount(uint32_t payload_len)
{
    // Numerator can go negative for tiny payloads at high SF; the max(...,0)
    // in the standard form is what handles that, so do the division on a
    // signed type and clamp before scaling.
    const int32_t num = (int32_t) (8u * payload_len)
                      - 4 * (int32_t) kSpreadingFactor
                      + 28
                      + (kCrcOn ? 16 : 0)
                      - (kImplicitHeader ? 20 : 0);
    const int32_t den = 4 * ((int32_t) kSpreadingFactor - (kLowDataRateOpt ? 2 : 0));

    // Ceiling division that is correct for a negative numerator too.
    // (CR + 4) in the standard form IS the coding-rate denominator: CR 4/8 has
    // CR = 4 and costs 8 symbols per block.
    const int32_t blocks = (num <= 0) ? 0 : ((num + den - 1) / den);

    return 8u + (uint32_t) (blocks * (int32_t) kCodingRateDenom);
}

// T0 -> RxDone, i.e. header + payload. This is the quantity the node subtracts.
constexpr uint32_t t0ToRxDoneUs(uint32_t payload_len)
{
    return symbolCount(payload_len) * kSymbolUs;
}

// Full time on air: air start -> RxDone. Per direction, because the preambles
// differ; there is deliberately no direction-less form to reach for.
constexpr uint32_t downlinkTimeOnAirUs(uint32_t payload_len)
{
    return kDownlinkPreambleToT0Us + t0ToRxDoneUs(payload_len);
}
constexpr uint32_t uplinkTimeOnAirUs(uint32_t payload_len)
{
    return kUplinkPreambleToT0Us + t0ToRxDoneUs(payload_len);
}

// The node's reference recovery, as ONE operation. Do not open-code this.
constexpr int64_t t0FromRxDoneUs(int64_t t_rxdone_us, uint32_t payload_len)
{
    return t_rxdone_us - (int64_t) t0ToRxDoneUs(payload_len);
}

// --- Channel-activity detection ------------------------------------------
//
// CAD duration at this PHY, from the datasheet's own decomposition: the
// receiver listens for one symbol (2^SF / BW) and then spends a further 32/BW
// correlating what it heard. Both terms are exact once SF and BW are fixed, so
// this is a DERIVATION, not an assumption — unlike kDetectSymbolsAssumed in
// TimedGrid.h, which is a rule of thumb wearing a number's clothes.
//
// It matters because the node transmits CAD-first: a frame aimed at an instant
// must begin its CAD this much earlier or it arrives a whole CAD late.
static constexpr uint32_t kCadUs =
    (uint32_t) ((((uint64_t) 1u << kSpreadingFactor) + 32ull) * 1000000ull
                / kBandwidthHz);

// The hub's fire instant for a wanted DOWNLINK T0. d_tx_ramp is the PLL/PA ramp between
// the RegOpMode=TX write and the first chirp leaving the antenna; it is
// UNMEASURED (implementation-plan.md section 12.1) and is passed in rather than
// guessed here, so no caller can mistake a placeholder for a measurement.
constexpr int64_t fireInstantUs(int64_t t0_us, uint32_t d_tx_ramp_us)
{
    return t0_us - (int64_t) kDownlinkPreambleToT0Us - (int64_t) d_tx_ramp_us;
}

// The instant a node's CAD-then-transmit sequence must BEGIN for the resulting
// UPLINK's T0 to land on `t0_us`: the uplink preamble, with the sender's own
// pre-transmit work in front of it:
//
//   cad_start ---kCadUs---> fire ---d_tx_ramp---> air start ---T_pre---> T0
//                 |
//                 +-- d_cad_dispatch: the software between deciding to send and
//                     the CAD actually running (task hop, radio mutex, the
//                     RegOpMode/DIO-mapping writes, the queue reset).
//
// d_cad_dispatch is UNMEASURED (test-plan.md HW-7's neighbour) and is passed in
// for the same reason d_tx_ramp is: a caller that supplies 0 is EARLY by
// nothing and LATE by exactly that term, which is a knowable error, whereas a
// constant guessed here would be an unknowable one.
constexpr int64_t cadStartInstantUs(int64_t t0_us, uint32_t d_cad_dispatch_us,
                                    uint32_t d_tx_ramp_us)
{
    return t0_us - (int64_t) kUplinkPreambleToT0Us - (int64_t) d_tx_ramp_us
         - (int64_t) kCadUs
         - (int64_t) d_cad_dispatch_us;
}

// And back. A producer that has already chosen a mark hands the queue a FIRE
// instant, and a consumer that has to reason about the mark again — which round
// it falls in, say — must not re-derive it by open-coding the offset with the
// wrong sign. That has happened once already, in both placement producers.
constexpr int64_t t0FromFireInstantUs(int64_t fire_us, uint32_t d_tx_ramp_us)
{
    return fire_us + (int64_t) kDownlinkPreambleToT0Us + (int64_t) d_tx_ramp_us;
}

// ---------------------------------------------------------------------------
// Compile-time pins. If a radio setting above is edited, these fail here rather
// than in the field as a window that is armed at the wrong microsecond.
// ---------------------------------------------------------------------------
static_assert(kSymbolUs == 256, "T_sym must be 256 us at SF7/BW500");
static_assert(kUplinkPreambleToT0Us == 3136, "uplink air start -> T0 must be 3136 us");
static_assert(kDownlinkPreambleToT0Us == 4160, "downlink air start -> T0 must be 4160 us");
static_assert(kHeaderUs == 2048, "T0 -> ValidHeader must be 2048 us");
static_assert(kCadUs == 320, "CAD must be 320 us at SF7/BW500");

static_assert(symbolCount(25) == 72,   "25 B ack");
static_assert(symbolCount(45) == 120,  "45 B beacon");
static_assert(symbolCount(60) == 152,  "60 B routine command");
static_assert(symbolCount(152) == 360, "152 B ScheduleConfig");

// The discontinuity, pinned so nobody "simplifies" the PL convention: 58 and 60
// bytes cost the same, 62 costs 8 symbols more.
static_assert(symbolCount(58) == symbolCount(60), "same symbol block");
static_assert(symbolCount(62) == symbolCount(60) + 8, "next symbol block");

static_assert(uplinkTimeOnAirUs(25)  == 21568, "25 B ack, uplink");
static_assert(uplinkTimeOnAirUs(60)  == 42048, "60 B uplink");
static_assert(downlinkTimeOnAirUs(45)  == 34880, "45 B beacon, downlink");
static_assert(downlinkTimeOnAirUs(60)  == 43072, "60 B routine command, downlink");
static_assert(downlinkTimeOnAirUs(152) == 96320, "152 B ScheduleConfig, downlink");

// A grid period must be a whole number of milliseconds — see kBurstCopyStrideUs.
static_assert(kBurstCopyStrideUs % 1000 == 0, "burst stride must be integer ms");

}  // namespace loratiming
