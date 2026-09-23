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
//       |<---------- kPreambleToT0Us ------>|
//
//   hub:  T0 = t_fire + d_tx_ramp + kPreambleToT0Us
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

// --- Radio configuration in force on this link ----------------------------
// SF7 / BW500 / CR4-8 / explicit header / CRC on. Changing any of these
// changes every timing constant below, which is why they are named here and
// the static_asserts at the bottom fail loudly if the derivation drifts.
static constexpr uint8_t  kSpreadingFactor   = 7;
static constexpr uint32_t kBandwidthHz       = 500000;
static constexpr uint8_t  kCodingRateDenom   = 8;   // CR 4/8 -> the "+4" term is 4
static constexpr uint8_t  kPreambleSymbols   = 8;
static constexpr bool     kCrcOn             = true;
static constexpr bool     kImplicitHeader    = false;
static constexpr bool     kLowDataRateOpt    = false;

// T_sym = 2^SF / BW. At SF7/BW500 this is exactly 256 us; the expression is
// kept general so a bandwidth change is a compile-time result, not a hand edit.
static constexpr uint32_t kSymbolUs =
    (uint32_t) (((uint64_t) 1u << kSpreadingFactor) * 1000000ull / kBandwidthHz);

// air start -> T0. The preamble is n_pre up-chirps plus 4.25 symbols of sync
// word and SFD. Expressed in quarters to stay in integer arithmetic.
static constexpr uint32_t kPreambleToT0Us =
    ((uint32_t) kPreambleSymbols * 4u + 17u) * kSymbolUs / 4u;

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

// Full time on air: air start -> RxDone.
constexpr uint32_t timeOnAirUs(uint32_t payload_len)
{
    return kPreambleToT0Us + t0ToRxDoneUs(payload_len);
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

// The hub's fire instant for a wanted T0. d_tx_ramp is the PLL/PA ramp between
// the RegOpMode=TX write and the first chirp leaving the antenna; it is
// UNMEASURED (implementation-plan.md section 12.1) and is passed in rather than
// guessed here, so no caller can mistake a placeholder for a measurement.
constexpr int64_t fireInstantUs(int64_t t0_us, uint32_t d_tx_ramp_us)
{
    return t0_us - (int64_t) kPreambleToT0Us - (int64_t) d_tx_ramp_us;
}

// The instant a CAD-then-transmit sequence must BEGIN for the resulting frame's
// T0 to land on `t0_us`. This is fireInstantUs() with the sender's own
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
    return fireInstantUs(t0_us, d_tx_ramp_us)
         - (int64_t) kCadUs
         - (int64_t) d_cad_dispatch_us;
}

// And back. A producer that has already chosen a mark hands the queue a FIRE
// instant, and a consumer that has to reason about the mark again — which round
// it falls in, say — must not re-derive it by open-coding the offset with the
// wrong sign. That has happened once already, in both placement producers.
constexpr int64_t t0FromFireInstantUs(int64_t fire_us, uint32_t d_tx_ramp_us)
{
    return fire_us + (int64_t) kPreambleToT0Us + (int64_t) d_tx_ramp_us;
}

// ---------------------------------------------------------------------------
// Compile-time pins. If a radio setting above is edited, these fail here rather
// than in the field as a window that is armed at the wrong microsecond.
// ---------------------------------------------------------------------------
static_assert(kSymbolUs == 256, "T_sym must be 256 us at SF7/BW500");
static_assert(kPreambleToT0Us == 3136, "air start -> T0 must be 3136 us");
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

static_assert(timeOnAirUs(25)  == 21568, "25 B time on air");
static_assert(timeOnAirUs(45)  == 33856, "45 B time on air");
static_assert(timeOnAirUs(60)  == 42048, "60 B time on air");
static_assert(timeOnAirUs(152) == 95296, "152 B time on air");

// A grid period must be a whole number of milliseconds — see kBurstCopyStrideUs.
static_assert(kBurstCopyStrideUs % 1000 == 0, "burst stride must be integer ms");

}  // namespace loratiming
