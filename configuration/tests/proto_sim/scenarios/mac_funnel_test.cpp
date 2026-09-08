// MacFunnel — the frame funnel and the three rates that get confused for
// each other.
//
// See docs/mac-layer.md section 6 and docs/test-plan.md section 10.4.

#include <gtest/gtest.h>

#include "MacFunnel.h"

using namespace macfunnel;

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

TEST(MacFunnel, StartsEmptyAndResets) {
    Counters c;
    EXPECT_EQ(c.detected, 0u);
    c.noteDetected(true);
    EXPECT_EQ(c.detected, 1u);
    c.reset();
    EXPECT_EQ(c.detected, 0u);
    EXPECT_EQ(c.crc_valid, 0u);
}

TEST(MacFunnel, DetectedSplitsIntoCrcValidAndErrors) {
    Counters c;
    c.noteDetected(true);
    c.noteDetected(false);
    c.noteDetected(true);
    EXPECT_EQ(c.detected, 3u);
    EXPECT_EQ(c.crc_valid, 2u);
    EXPECT_EQ(c.crc_errors, 1u);
    // The invariant that makes 2->3 the only real loss transition.
    EXPECT_EQ(c.crc_valid + c.crc_errors, c.detected);
}

TEST(MacFunnel, FilteringIsNotLoss) {
    // On a shared broadcast channel a node hears its neighbours constantly.
    // Discarding their traffic is the filter working, so these must not feed
    // any error rate.
    Counters c;
    for (int i = 0; i < 10; ++i) c.noteDetected(true);
    c.noteAddressed(false);
    c.noteAddressed(false);
    c.noteAddressed(true);

    EXPECT_EQ(c.foreign, 2u);
    EXPECT_EQ(c.addressed, 1u);
    EXPECT_EQ(ferLinkPpm(c, 10), 0u) << "foreign frames must not read as loss";
}

// ---------------------------------------------------------------------------
// The three rates, named apart
// ---------------------------------------------------------------------------

TEST(MacFunnel, FerAirAndFerLinkDivergeExactlyByTransmitterLoss) {
    // The hub offered 100 marks; only 90 reached the air (ten RegOpMode=TX
    // writes silently skipped on a one-tick semaphore timeout); of those 90 the
    // node heard 81.
    Counters c;
    for (int i = 0; i < 81; ++i) c.noteDetected(true);

    EXPECT_EQ(ferAirPpm(c, 90), 100000u);    // 10 % — the channel
    EXPECT_EQ(ferLinkPpm(c, 100), 190000u);  // 19 % — channel plus the hub

    // The difference is the transmitter's own loss, and it is visible ONLY with
    // a witness receiver. Without one the two collapse and "the hub did not
    // send it" is indistinguishable from "the node did not hear it".
    EXPECT_GT(ferLinkPpm(c, 100), ferAirPpm(c, 90));
}

TEST(MacFunnel, FerIsUnchangedByWindowMisses) {
    // FER is a property of the CHANNEL. If it moved when the window policy
    // changed, the two arms of a mode comparison would not be comparable — and
    // that check is B3's gate, not a footnote.
    Counters a, b;
    for (int i = 0; i < 50; ++i) { a.noteDetected(true); b.noteDetected(true); }
    for (int i = 0; i < 10; ++i) a.noteWindow(true);
    for (int i = 0; i < 10; ++i) b.noteWindow(i < 5);

    EXPECT_EQ(ferLinkPpm(a, 60), ferLinkPpm(b, 60));
    EXPECT_NE(wmrPpm(a), wmrPpm(b));
}

TEST(MacFunnel, WmrIsTheRateThatDistinguishesModes) {
    Counters c;
    for (int i = 0; i < 100; ++i) c.noteWindow(i < 96);
    EXPECT_EQ(c.windows_armed, 100u);
    EXPECT_EQ(c.windows_hit, 96u);
    EXPECT_EQ(wmrPpm(c), 40000u);   // 4 %
}

TEST(MacFunnel, DuplicatesAreHealthyInModeA) {
    // A 17-copy burst is sixteen intentional duplicates. Folding that into
    // frame loss would make the safest mode look like the worst one.
    Counters c;
    for (int i = 0; i < 17; ++i) c.noteDetected(true);   // all 17 copies heard
    c.noteCounter(true);
    for (int i = 0; i < 16; ++i) c.noteCounter(false);

    EXPECT_EQ(c.counter_accepted, 1u);
    EXPECT_EQ(c.duplicates, 16u);
    EXPECT_NEAR((double) dupPpm(c), 941176.0, 1.0);   // 16/17

    // 94 % duplicates and ZERO loss, from the same run. That pairing is the
    // whole reason DUP is its own rate.
    EXPECT_EQ(ferLinkPpm(c, 17), 0u);
}

TEST(MacFunnel, MicFailureIsNeverAChannelEffect) {
    // A corrupted frame fails CRC long before it reaches the MIC, so a non-zero
    // rate here is a session bug and should be read as one.
    Counters c;
    for (int i = 0; i < 9; ++i) c.noteMic(true);
    c.noteMic(false);
    EXPECT_EQ(micFailPpm(c), 100000u);
    EXPECT_EQ(c.crc_errors, 0u) << "MIC failures happen on frames that passed CRC";
}

// ---------------------------------------------------------------------------
// Arithmetic edges
// ---------------------------------------------------------------------------

TEST(MacFunnel, ZeroDenominatorIsZeroNotAFault) {
    Counters c;
    EXPECT_EQ(ferAirPpm(c, 0), 0u);
    EXPECT_EQ(ferLinkPpm(c, 0), 0u);
    EXPECT_EQ(wmrPpm(c), 0u);
    EXPECT_EQ(dupPpm(c), 0u);
    EXPECT_EQ(micFailPpm(c), 0u);
}

TEST(MacFunnel, NoFramesOfferedIsNotNoErrors) {
    // The counters travel WITH the rate precisely so a reader can tell "0 %
    // loss" from "nothing was measured". Asserting the pairing, not the value.
    Counters c;
    EXPECT_EQ(ferLinkPpm(c, 0), 0u);
    EXPECT_EQ(c.detected, 0u) << "a zero rate with a zero denominator is not a "
                                 "result, and the caller must be able to see that";
}

TEST(MacFunnel, HearingMoreThanWasOfferedDoesNotUnderflow) {
    // Retransmissions and a stale offered count can make good > total. Must
    // clamp to zero rather than wrap a uint32 to ~4 billion ppm.
    Counters c;
    for (int i = 0; i < 20; ++i) c.noteDetected(true);
    EXPECT_EQ(ferLinkPpm(c, 10), 0u);
}

TEST(MacFunnel, RatesAreExactAtRoundFractions) {
    Counters c;
    for (int i = 0; i < 75; ++i) c.noteDetected(true);
    EXPECT_EQ(ferLinkPpm(c, 100), 250000u);
    EXPECT_EQ(lossPpm(1, 3), 666666u);   // truncates, never rounds up
    EXPECT_EQ(lossPpm(0, 1), 1000000u);
}

TEST(MacFunnel, LargeCountsDoNotOverflow) {
    // A week at one frame per 1.5 s is ~400k frames; the ppm scaling must be
    // done in 64-bit or it wraps well before that.
    Counters c;
    c.crc_valid = 3000000;
    EXPECT_EQ(ferLinkPpm(c, 4000000), 250000u);
}

// ---------------------------------------------------------------------------
// Armed and hit as separate events
// ---------------------------------------------------------------------------

TEST(MacFunnel, AWindowThatCaughtSomethingUnusableStillCountsAsArmed) {
    // The flattering-WMR trap. A window that opens and then receives a frame
    // which fails CRC, fails to parse, or is addressed to another node has
    // still been armed — the battery was spent. Counting armed only at the
    // address filter would drop those out of the denominator.
    macfunnel::Counters c;
    c.noteWindowArmed();
    c.noteWindowArmed();
    c.noteWindowArmed();
    c.noteWindowHit();          // only one of the three produced a usable frame

    EXPECT_EQ(c.windows_armed, 3u);
    EXPECT_EQ(c.windows_hit, 1u);
    EXPECT_EQ(macfunnel::wmrPpm(c), 666666u) << "two thirds missed";
}

TEST(MacFunnel, TheSplitFormAgreesWithTheCombinedOne) {
    macfunnel::Counters split, combined;
    for (bool hit : {true, false, true, true, false}) {
        split.noteWindowArmed();
        if (hit) split.noteWindowHit();
        combined.noteWindow(hit);
    }
    EXPECT_EQ(split.windows_armed, combined.windows_armed);
    EXPECT_EQ(split.windows_hit, combined.windows_hit);
    EXPECT_EQ(macfunnel::wmrPpm(split), macfunnel::wmrPpm(combined));
}

TEST(MacFunnel, NoWindowsArmedIsNotAPerfectScore) {
    // WMR with a zero denominator must read 0 as "not measured", and the caller
    // has to check windows_armed to tell that apart from "nothing missed".
    // Until this session nothing called the arming side at all, so this was the
    // permanent state of the KPI the design calls the one that distinguishes
    // the three modes.
    macfunnel::Counters c;
    EXPECT_EQ(c.windows_armed, 0u);
    EXPECT_EQ(macfunnel::wmrPpm(c), 0u);
}
