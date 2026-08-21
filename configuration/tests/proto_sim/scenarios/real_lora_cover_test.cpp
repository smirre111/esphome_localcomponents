// Phase 3 — exercise the REAL production LoraCoverComponent against host
// shims for cover::Cover / cover::CoverCall / cover::CoverTraits.
//
// The class under test is esphome::loracov::LoraCoverComponent defined in
// local_components/loracover/cover/lora_cover.h. It inherits cover::Cover,
// LORAClientNode and Component; the shims supply the Cover surface.
//
// The control() method is protected in production; we wrap it through a
// minimal subclass that exposes a public dispatch path so the test can
// drive CMD_OPERATION / position-set messages and inspect the wire
// transcript.

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/loracover/cover/lora_cover.h"
#include "esphome/components/cover/cover.h"

#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/wire_codec.h"

#include <gtest/gtest.h>

using esphome::lora_tracker::LORAClient;
using esphome::lora_tracker::LORATracker;
using esphome::loracov::LoraCoverComponent;

namespace {

constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

// LoraCoverComponent::control is protected in production. A thin test
// subclass forwards a CoverCall through it so scenarios can drive
// CMD_STOP / CMD_OPEN / CMD_CLOSE / position-set requests.
class TestableCover : public LoraCoverComponent {
public:
    void drive(const esphome::cover::CoverCall& call) {
        this->control(call);
    }
};

struct CoverHarness {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker  tracker;
    LORAClient   rol;
    TestableCover cover;

    CoverHarness() {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

        rol.set_name("rol_2");
        rol.set_short_address(18);
        rol.set_subnet_address(2);
        rol.set_sleep_duration(21600);
        rol.set_address(kMacRol2);
        tracker.register_client(&rol);
        rol.registered_ = true;

        cover.set_name("rol_2_cover");
        cover.set_open_duration(60);
        cover.set_close_duration(60);
        cover.set_invert_position(false);
        cover.set_blind_height_mm(2000.0f);
        cover.set_axle_diameter_mm(60.0f);
        cover.set_blind_thickness_mm(8.0f);
        cover.setup();
        rol.register_lora_node(&cover);
    }
    ~CoverHarness() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

// D1 against real LoraCoverComponent: an open-via-position call puts a
// CMD_OPERATION with destAddress=18 on the wire — directly stressing the
// "every node addressed as 17" misrouting class.
TEST(RealLoraCover, OpenForRol2RoutesToAddr18OnWire) {
    CoverHarness h;
    esphome::cover::CoverCall call;
    call.set_position(esphome::cover::COVER_OPEN);
    h.cover.drive(call);

    bool found = false;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (!m) continue;
        if (m->cmd != proto_sim::LoraClientOperationMessage::Cmd::Operation) continue;
        EXPECT_EQ(m->header.destAddress, 18u)
            << "Real LoraCoverComponent::control() must put destAddress=18 "
               "on the wire — guard against the original misrouting bug.";
        found = true;
    }
    EXPECT_TRUE(found) << "control(set_position(OPEN)) must emit a CMD_OPERATION";
}

TEST(RealLoraCover, StopEmitsStopOperation) {
    CoverHarness h;
    esphome::cover::CoverCall call;
    call.set_stop(true);
    h.cover.drive(call);

    int stop_count = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (!m) continue;
        if (m->cmd != proto_sim::LoraClientOperationMessage::Cmd::Operation) continue;
        if (m->operation.kind == proto_sim::LoraCoverOperation::Kind::Operation &&
            m->operation.operation == proto_sim::CovOperation::CMD_STOP) {
            EXPECT_EQ(m->header.destAddress, 18u);
            ++stop_count;
        }
    }
    EXPECT_EQ(stop_count, 1)
        << "Real control(set_stop) must emit exactly one CMD_STOP.";
}

// Real send_remote_config: covers the LoraCoverComponent's outbound
// CoverConfig path, including all three geometry fields.
TEST(RealLoraCover, SendRemoteConfigEmitsCoverConfigWithGeometry) {
    CoverHarness h;
    h.cover.send_remote_config();

    int cfg_count = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (!m) continue;
        if (m->cmd != proto_sim::LoraClientOperationMessage::Cmd::CoverConfig) continue;
        EXPECT_EQ(m->coverconfig.openTime,  60u);
        EXPECT_EQ(m->coverconfig.closeTime, 60u);
        EXPECT_FLOAT_EQ(m->coverconfig.blindHeightMm,    2000.0f);
        EXPECT_FLOAT_EQ(m->coverconfig.axleDiameterMm,     60.0f);
        EXPECT_FLOAT_EQ(m->coverconfig.blindThicknessMm,    8.0f);
        EXPECT_EQ(m->header.destAddress, 18u);
        ++cfg_count;
    }
    EXPECT_EQ(cfg_count, 1)
        << "Real send_remote_config() must emit exactly one CoverConfig "
           "with destAddress=18 and full geometry.";
}

} // namespace
