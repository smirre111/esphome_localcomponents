// Review HB23: MAC-0 demultiplexes uplinks by SRC once, instead of every
// listener decoding every frame.
#include "UplinkDemux.h"

#include <gtest/gtest.h>
#include <vector>

namespace {

std::vector<uint8_t> frame(uint8_t src, uint8_t port, macwire::Auth auth, size_t body = 4) {
    macwire::WireHdr h;
    h.flags  = macwire::withAuth(0, auth);
    h.dst    = 1;
    h.src    = src;
    h.subnet = 2;
    h.fcnt   = 7;
    h.port   = port;
    const size_t hlen = macwire::headerLen(h);
    const size_t tag  = (auth == macwire::Auth::None) ? 0 : macwire::kTagBytes;
    std::vector<uint8_t> f(hlen + body + tag, 0xA5);
    EXPECT_TRUE(macwire::encodeHeader(h, f.data(), f.size()));
    return f;
}

struct L { uint8_t addr; int got{0}; };

size_t run(const std::vector<uint8_t> &f, std::vector<L> &ls) {
    return uplinkdemux::dispatch(f.data(), f.size(), ls,
                                 [](const L &l) { return l.addr; },
                                 [](const L &l) { const_cast<L &>(l).got++; });
}

}  // namespace

TEST(UplinkDemux, AFrameGoesOnlyToTheListenerItCameFrom) {
    std::vector<L> ls{{17}, {18}, {19}};
    EXPECT_EQ(run(frame(18, macwire::kPortApp, macwire::Auth::Session), ls), 1u);
    EXPECT_EQ(ls[0].got, 0);
    EXPECT_EQ(ls[1].got, 1);
    EXPECT_EQ(ls[2].got, 0);
}

TEST(UplinkDemux, AFrameFromAnUnknownSenderGoesNowhere) {
    std::vector<L> ls{{17}, {18}};
    EXPECT_EQ(run(frame(42, macwire::kPortMac0Mgmt, macwire::Auth::None), ls), 0u);
}

TEST(UplinkDemux, ARegisterGoesToEveryListenerToBeMatchedOnItsMac) {
    // A REGISTER's sender address may be new or about to change; the listeners
    // match it on the factory MAC inside the body.
    std::vector<L> ls{{17}, {18}, {19}};
    EXPECT_EQ(run(frame(0xFE, macwire::kPortSession, macwire::Auth::KAuth), ls), 3u);
    for (const L &l : ls) EXPECT_EQ(l.got, 1);
}

TEST(UplinkDemux, OnlyAKAuthFrameOnPortOneIsTreatedAsARegister) {
    std::vector<L> ls{{17}, {18}};
    EXPECT_EQ(run(frame(18, macwire::kPortSession, macwire::Auth::None), ls), 1u)
        << "a plaintext PORT 1 frame is not a REGISTER: routed by SRC like any other";
    EXPECT_EQ(run(frame(18, macwire::kPortApp, macwire::Auth::KAuth), ls), 1u)
        << "K_AUTH on another port is not a REGISTER either";
}

TEST(UplinkDemux, BytesThatAreNotAV1FrameGoNowhere) {
    std::vector<L> ls{{17}, {18}};
    std::vector<uint8_t> junk{0x00, 0x11, 0x22};
    EXPECT_EQ(run(junk, ls), 0u);
    EXPECT_EQ(uplinkdemux::route(nullptr, 0).route, uplinkdemux::Route::Drop);
}

TEST(UplinkDemux, TwoListenersSharingAnAddressBothGetTheFrame) {
    // A configuration error, but not one MAC-0 should hide by picking one.
    std::vector<L> ls{{18}, {18}};
    EXPECT_EQ(run(frame(18, macwire::kPortApp, macwire::Auth::Session), ls), 2u);
}
