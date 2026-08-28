// SessionManager — the session state extracted out of CmdDispatcher.
//
// These are the cases that previously required a node on a roof. The state was
// reachable only by constructing a whole CmdDispatcher with its motor, radio
// and mutexes, so three real defects hid in it and were all found on hardware:
//
//   * the persist peer never tracked the hub, so saves rewrote an ancient
//     broadcast-keyed blob and the resume beacon carried a nonce the hub had
//     never held (psa_aead_decrypt failed: -149, twice, in production);
//   * a zero nonce was reported as a usable session;
//   * the restored hub address was truncated through a uint8_t cast.
//
// SessionManager is constructible in one line. That is the entire point.

#include <gtest/gtest.h>

#include "SessionManager.h"
#include "nvs.h"

namespace {

constexpr uint32_t kHub   = 1;        // the hub is peer 1 in production
constexpr uint32_t kOther = 17;       // another node on the same air
constexpr uint32_t kNonce = 0xA1B2C3D4;

SessionManager freshManager() {
    proto_sim_nvs_reset();
    return SessionManager{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Base nonces
// ---------------------------------------------------------------------------

TEST(SessionManager, UnknownPeerHasNoSession) {
    SessionManager s = freshManager();
    uint32_t out = 0xDEAD;
    EXPECT_FALSE(s.getBaseNonce(kHub, out));
}

TEST(SessionManager, ZeroNonceIsNotASession) {
    SessionManager s = freshManager();
    // Creating counters for a peer must not imply a session with it: a peer
    // entry is created the moment we track anything for that address, with a
    // zero nonce. Reporting that as usable let the node encrypt a resume beacon
    // the hub could not authenticate.
    s.setBaseNonce(kHub, 0);

    uint32_t out = 0xDEAD;
    EXPECT_FALSE(s.getBaseNonce(kHub, out))
        << "a zero nonce is the cleared/never-set sentinel, not a session";
    EXPECT_EQ(out, 0xDEADu) << "the out-parameter must be left untouched";
}

TEST(SessionManager, ARealNonceIsASession) {
    SessionManager s = freshManager();
    s.setBaseNonce(kHub, kNonce);

    uint32_t out = 0;
    ASSERT_TRUE(s.getBaseNonce(kHub, out));
    EXPECT_EQ(out, kNonce);
}

TEST(SessionManager, ClearingRemovesTheSession) {
    SessionManager s = freshManager();
    s.setBaseNonce(kHub, kNonce);
    s.clearBaseNonce(kHub);

    uint32_t out = 0;
    EXPECT_FALSE(s.getBaseNonce(kHub, out));
}

TEST(SessionManager, PeersDoNotShareNonces) {
    // Two nodes share the air; one node's session must never satisfy a lookup
    // for another's.
    SessionManager s = freshManager();
    s.setBaseNonce(kOther, kNonce);

    uint32_t out = 0;
    EXPECT_FALSE(s.getBaseNonce(kHub, out));
    EXPECT_TRUE(s.getBaseNonce(kOther, out));
}

TEST(SessionManager, OnlyThePersistPeerAsksToBeSaved) {
    SessionManager s = freshManager();
    s.setPersistPeer(kHub);

    EXPECT_TRUE(s.setBaseNonce(kHub, kNonce))
        << "the hub's nonce is the critical secret — it must be persisted "
           "immediately whenever it changes";
    EXPECT_FALSE(s.setBaseNonce(kOther, kNonce))
        << "another node's nonce must not trigger a write of OUR blob";
}

// ---------------------------------------------------------------------------
// Replay window
// ---------------------------------------------------------------------------

TEST(SessionManager, AcceptsAForwardJumpAndAdvances) {
    SessionManager s = freshManager();
    EXPECT_TRUE(s.acceptRxId(5));
    EXPECT_EQ(s.rxId(), 5u);
    EXPECT_TRUE(s.acceptRxId(6));
    EXPECT_EQ(s.rxId(), 6u);
}

TEST(SessionManager, RejectsAReplayAndLeavesTheCounterAlone) {
    SessionManager s = freshManager();
    ASSERT_TRUE(s.acceptRxId(10));

    EXPECT_FALSE(s.acceptRxId(10)) << "same id is a replay";
    EXPECT_FALSE(s.acceptRxId(9))  << "older id is a replay";
    EXPECT_EQ(s.rxId(), 10u)
        << "a rejected frame must not move the counter — that is how an "
           "overheard frame used to wedge the link";
}

TEST(SessionManager, RejectsAJumpBeyondTheWindow) {
    SessionManager s = freshManager();
    ASSERT_TRUE(s.acceptRxId(10));

    // Ratcheting onto a corrupt id would drop every legitimate lower id until
    // the next login.
    EXPECT_FALSE(s.acceptRxId(10 + SessionManager::kMsgIdWindow + 1));
    EXPECT_EQ(s.rxId(), 10u);

    // The window edge itself is still accepted.
    EXPECT_TRUE(s.acceptRxId(10 + SessionManager::kMsgIdWindow));
}

TEST(SessionManager, LoginResetsBothDirections) {
    SessionManager s = freshManager();
    ASSERT_TRUE(s.acceptRxId(50));
    s.nextTxId();
    s.nextTxId();
    ASSERT_EQ(s.txId(), 2u);

    s.resetCounters();
    EXPECT_EQ(s.rxId(), 0u);
    EXPECT_EQ(s.txId(), 0u)
        << "the hub zeroes its counters in send_login(); the node must match "
           "or every subsequent frame is judged against the wrong sequence";
}

TEST(SessionManager, TxIdsAreConsumedNotRepeated) {
    SessionManager s = freshManager();
    EXPECT_EQ(s.nextTxId(), 1u);
    EXPECT_EQ(s.nextTxId(), 2u);
    EXPECT_EQ(s.txId(), 2u) << "txId() peeks, it must not consume";
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

TEST(SessionManager, NoBlobMeansNoValidState) {
    SessionManager s = freshManager();
    s.load();
    EXPECT_FALSE(s.hasValidState());
}

TEST(SessionManager, ASessionSurvivesSaveAndLoad) {
    proto_sim_nvs_reset();
    {
        SessionManager a;
        a.setPersistPeer(kHub);
        a.setBaseNonce(kHub, kNonce);
        for (int i = 0; i < 7; i++) a.nextTxId();
        ASSERT_TRUE(a.acceptRxId(3));
        a.save();
    }

    SessionManager b;
    b.load();

    ASSERT_TRUE(b.hasValidState());
    EXPECT_EQ(b.persistPeer(), kHub)
        << "the restored blob must point at the HUB — pointing at 0xFF is what "
           "made every resume beacon undecryptable";
    uint32_t out = 0;
    ASSERT_TRUE(b.getBaseNonce(kHub, out));
    EXPECT_EQ(out, kNonce);
    EXPECT_EQ(b.rxId(), 3u);
    EXPECT_EQ(b.txId(), 7u + SessionManager::kPersistTxMargin)
        << "tx is restored with a reservation margin so increments that were "
           "never persisted can never be reused";
}

TEST(SessionManager, SavingWithoutASessionWritesNothing) {
    // The failure that produced blobs restoring a session the hub could never
    // authenticate: persist peer set, but no nonce for it.
    proto_sim_nvs_reset();
    {
        SessionManager a;
        a.setPersistPeer(kHub);
        a.nextTxId();
        a.save();
    }

    SessionManager b;
    b.load();
    EXPECT_FALSE(b.hasValidState())
        << "there is no session to save, so nothing should have been written";
}

TEST(SessionManager, SavingUnderTheWrongPeerWritesNothing) {
    // Exactly the persistHubAddr_ bug: the live nonce is filed under the hub,
    // but the persist peer still points somewhere else.
    proto_sim_nvs_reset();
    {
        SessionManager a;
        a.setBaseNonce(kHub, kNonce);   // real session with the hub
        a.setPersistPeer(0xFF);         // ...but we persist for 0xFF
        a.save();
    }

    SessionManager b;
    b.load();
    EXPECT_FALSE(b.hasValidState())
        << "a blob keyed to a peer we have no session with is worse than no "
           "blob: it restores confidently and cannot decrypt anything";
}

TEST(SessionManager, ThrottledPersistOnlyWritesAfterAFullMargin) {
    proto_sim_nvs_reset();
    SessionManager a;
    a.setPersistPeer(kHub);
    a.setBaseNonce(kHub, kNonce);   // this itself does not write; caller decides

    a.maybePersist();               // tx is 0 — nothing to write yet
    {
        SessionManager probe;
        probe.load();
        EXPECT_FALSE(probe.hasValidState()) << "no write expected this early";
    }

    for (uint32_t i = 0; i < SessionManager::kPersistTxMargin; i++) a.nextTxId();
    a.maybePersist();               // now a full margin has elapsed

    SessionManager probe;
    probe.load();
    EXPECT_TRUE(probe.hasValidState())
        << "NVS wear is bounded by writing once per margin, but it must "
           "actually write when the margin is reached";
}
