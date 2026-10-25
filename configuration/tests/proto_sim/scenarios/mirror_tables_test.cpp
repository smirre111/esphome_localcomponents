// Mirror gates (review P2.5 / P3, 2026-10-25).
//
// Three pieces of the hub<->node contract are kept by hand on both sides and
// are NOT vendored, so the header drift gates cannot see them:
//   * HubFrames.h (hub) mirrors NodeFrames.h (node): the internal case numbers
//     and the header/ack field layout "field for field the same names";
//   * HubWire.cpp kRoutes (hub: PORT + ACK_REQ per downlink case) and
//     PortMap.h kDownlinkPorts (node: port + "acked when executed");
//   * PortTable.h (hub: uplink case -> port) and MacWirePdu.h uplinkPort (node).
// A disagreement compiles and links on both sides and fails only on the air --
// e.g. a kind the hub retries until acked that the node never acks ends in a
// session teardown. This file makes each one a build error or a failing test.

#include "PortMap.h"       // node
#include "MacWirePdu.h"    // node
#include "NodeFrames.h"    // node

#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/HubWire.h"
#include "esphome/components/lora_client/PortTable.h"

#include <gtest/gtest.h>
#include <cstddef>

// ---- case numbers: every named enumerator, both sides ----------------------
#define SAME_CASE(name) static_assert(static_cast<int>(hubframes::name) == static_cast<int>(::name), \
                                      #name " differs between HubFrames.h and NodeFrames.h")
SAME_CASE(DL_NOT_SET);
SAME_CASE(DL_OPERATION);
SAME_CASE(DL_SYSOP);
SAME_CASE(DL_CLIENTCONFIG);
SAME_CASE(DL_COVERCONFIG);
SAME_CASE(DL_LOGIN);
SAME_CASE(DL_TIMESYNC);
SAME_CASE(DL_SCHEDULE);
SAME_CASE(DL_DRIFTTEST);
SAME_CASE(DL_MACCONTROL);
SAME_CASE(DL_GRIDSYNC);
SAME_CASE(DL_MODETEST);
SAME_CASE(DL_GRIDBEACON);
SAME_CASE(DL_GRIDDEMOTE);
SAME_CASE(UL_NOT_SET);
SAME_CASE(UL_SEALED);
SAME_CASE(UL_AVAIL);
SAME_CASE(UL_REGISTER);
SAME_CASE(UL_STATE);
SAME_CASE(UL_POSITION);
SAME_CASE(UL_ACK);
SAME_CASE(UL_BEACON);
SAME_CASE(UL_MACCONTROL);
SAME_CASE(UL_MODETESTREPORT);
SAME_CASE(UL_GRIDSYNCREQUEST);
#undef SAME_CASE

// ---- field layout: same names at the same offsets ---------------------------
#define SAME_FIELD(H, N, f) static_assert(offsetof(H, f) == offsetof(N, f), \
                                          #f " moved between " #H " and " #N)
static_assert(sizeof(hubframes::Hdr) == sizeof(MacFrameHdr), "Hdr / MacFrameHdr differ");
SAME_FIELD(hubframes::Hdr, MacFrameHdr, destaddress);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, destsubnet);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, senderaddress);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, msgid);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, burstindex);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, burstcount);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, onmark);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, firestamped);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, fireround);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, fireoffsetus);
SAME_FIELD(hubframes::Hdr, MacFrameHdr, sessionnonce);
static_assert(sizeof(hubframes::UpAck) == sizeof(UplinkAck), "UpAck / UplinkAck differ");
SAME_FIELD(hubframes::UpAck, UplinkAck, ack_msg_id);
SAME_FIELD(hubframes::UpAck, UplinkAck, status);
SAME_FIELD(hubframes::UpAck, UplinkAck, phase);
static_assert(sizeof(hubframes::Down) == sizeof(DownlinkView), "Down / DownlinkView differ");
SAME_FIELD(hubframes::Down, DownlinkView, cmd_case);
SAME_FIELD(hubframes::Down, DownlinkView, sysop);
#undef SAME_FIELD

namespace {
constexpr int kMaxCase = 64;   // above every oneof number in blinds.proto
}

// Every downlink case the hub frames has the node's port, and the hub sets
// ACK_REQ exactly for the kinds the node acks once executed. LOGIN is the one
// case the hub frames outside the route table (a K_AUTH bootstrap frame).
TEST(MirrorTables, EveryDownlinkCaseHasThePortAndAckRuleTheNodeHas) {
    int checked = 0;
    for (int c = 0; c < kMaxCase; ++c) {
        const hubwire::Route r = hubwire::routeDownlink(static_cast<hubframes::DownCase>(c));
        mac::Port p{};
        const bool node_has = portmap::portOf(static_cast<DownlinkCase>(c), &p);
        if (c == hubframes::DL_LOGIN) {
            EXPECT_FALSE(r.ok) << "LOGIN is framed by v1_login_frame_, not the route table";
            ASSERT_TRUE(node_has);
            EXPECT_EQ(p, mac::Port::Session);
            EXPECT_FALSE(portmap::ackedWhenExecuted(DL_LOGIN));
            continue;
        }
        EXPECT_EQ(r.ok, node_has) << "case " << c << " has a port on one side only";
        if (!r.ok || !node_has)
            continue;
        ++checked;
        EXPECT_EQ(r.port, static_cast<uint8_t>(p)) << "case " << c << ": PORT differs";
        EXPECT_EQ(r.ack_req, portmap::ackedWhenExecuted(static_cast<DownlinkCase>(c)))
            << "case " << c << ": the hub's ACK_REQ and the node's 'acked' table disagree";
    }
    EXPECT_EQ(checked, 12) << "every routed downlink case was compared";
}

// Every uplink case the node sends on a port is classified to that port by the
// hub (PortTable.h defaults unknown cases to App, so a case added on the node
// and forgotten here would be handed to the application children).
TEST(MirrorTables, EveryUplinkCaseIsClassifiedToThePortTheNodeSendsItOn) {
    int checked = 0;
    for (int c = 1; c < kMaxCase; ++c) {
        if (c == hubframes::UL_SEALED)
            continue;   // a test/hub view, never a body case
        uint8_t node_port = 0;
        if (!macwirepdu::uplinkPort(static_cast<UplinkCase>(c), &node_port))
            continue;
        ++checked;
        EXPECT_EQ(static_cast<uint8_t>(porttable::uplinkPort(static_cast<hubframes::UpCase>(c))),
                  node_port)
            << "uplink case " << c;
    }
    EXPECT_EQ(checked, 9) << "every uplink case the node sends was compared";
}
