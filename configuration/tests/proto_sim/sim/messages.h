#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// Plain-struct mirror of blinds.proto.
// Field names match the .proto and the generated pb-c symbols so the diff
// stays obvious.  wire_codec.{h,cpp} translates these <-> real protobuf-c
// bytes, so the on-air format under test is byte-identical to production.
//
// KEEP IN SYNC WITH blinds.proto.  The schema_drift_* CTest gates catch a
// stale .pb-c stub, but they cannot catch a stale mirror struct — if you add
// a proto field, add it here and in wire_codec.cpp too.

namespace proto_sim {

enum class CovOperation : uint32_t {
    CMD_OPEN  = 0,
    CMD_CLOSE = 1,
    CMD_STOP  = 2,
};

enum class ClientOperation : uint32_t {
    CMD_ENABLE_WIFI      = 0,
    CMD_DISABLE_WIFI     = 1,
    CMD_OTA              = 2,
    CMD_STATUS           = 3,
    CMD_SLEEP            = 4,
    // Auto-mode (P0): immediate mode transition for an awake node.
    CMD_MODE_AUTO        = 5,
    CMD_MODE_INTERACTIVE = 6,
};

enum class EncryptionAlgo : uint32_t {
    ENC_NONE        = 0,
    ENC_AES_GCM_128 = 1,
};

enum class AckStatus : uint32_t {
    ACK_OK    = 0,
    ACK_ERROR = 1,
    ACK_BUSY  = 2,
};

// ---- auto-mode additions (P0) ----

enum class SchedAction : uint32_t {
    SCHED_OPEN     = 0,
    SCHED_CLOSE    = 1,
    SCHED_STOP     = 2,
    SCHED_POSITION = 3,
};

enum class NodeMode : uint32_t {
    MODE_INTERACTIVE = 0,
    MODE_AUTO        = 1,
};

enum class WakeReason : uint32_t {
    WAKE_BOOT          = 0,
    WAKE_TIMER_EVENT   = 1,
    WAKE_TIMER_CHECKIN = 2,
    WAKE_BUTTON        = 3,
    WAKE_UNKNOWN       = 4,
};

struct TimeSync {
    uint64_t epoch{0};
    int32_t  utcOffset{0};
    uint64_t dstNext{0};
};

struct ScheduleEntry {
    uint32_t    minuteOfDay{0};
    uint32_t    dayMask{0};
    SchedAction action{SchedAction::SCHED_OPEN};
    uint32_t    positionPct{0};
    uint32_t    kind{0};
};

struct ScheduleConfig {
    uint32_t version{0};
    NodeMode mode{NodeMode::MODE_INTERACTIVE};
    uint32_t interactiveTimeout_s{0};
    uint32_t checkinInterval_s{0};
    uint32_t beaconLead_s{0};
    uint32_t postEventWindow_s{0};
    uint32_t catchupWindow_s{0};
    std::vector<ScheduleEntry> entries;
};

struct NodeWakeBeacon {
    WakeReason reason{WakeReason::WAKE_BOOT};
    uint32_t   schedVersion{0};
    uint64_t   nodeEpoch{0};
    NodeMode   mode{NodeMode::MODE_INTERACTIVE};
    float      voltage{0.0f};
    float      position{0.0f};
    uint32_t   awakeWindow_ms{0};
    uint64_t   nextEventEpoch{0};
    bool       sessionResume{false};
    bool       clockValid{false};
    uint32_t   fwVersion{0};
};

// ---- existing messages ----

struct LoraHeader {
    uint32_t destAddress{0};
    uint32_t destSubnet{0};
    uint32_t senderAddress{0};
    uint32_t msgId{0};
    // NOTE: proto field 5 (`encrypted`) was REMOVED — encryption is inferred
    // from the oneof case (presence of the `encrypted` payload).  Do not
    // reintroduce it here.
    // Burst scheduling (hub -> node); burstCount == 0 means single-shot.
    uint32_t burstIndex{0};
    uint32_t burstCount{0};
};

struct LoraCoverOperation {
    enum class Kind { Operation, Position } kind{Kind::Operation};
    CovOperation operation{CovOperation::CMD_STOP};
    float position{0.0f};
};

struct ClientConfig {
    uint64_t mac_addr{0};
    uint32_t addr{0};
    uint32_t subnt{0};
    std::string name;
    uint64_t sleepDuration{0};
    uint32_t batteryInterval{0};
};

struct CoverConfig {
    uint32_t openTime{0};
    uint32_t closeTime{0};
    float    blindHeightMm{0.0f};
    float    axleDiameterMm{0.0f};
    float    blindThicknessMm{0.0f};
    uint32_t openSlack{0};
    uint32_t closeSlack{0};
};

struct LoginMsg {
    uint32_t nonce{0};
    bool     request_register{false};
};

struct BaseNonceExchange {
    std::vector<uint8_t> key_id;
    uint32_t base_nonce{0}; // we encode as uint32 here; wire is 4-byte BE
};

// Slim on-air AEAD envelope: algorithm is fixed and the IV/AAD are
// reconstructed by the receiver from the plaintext outer header, so ONLY the
// tag and ciphertext are transmitted.  (`algo`, `iv` and `aad` were removed
// from the proto — do not reintroduce them here.)
struct EncryptedPayload {
    std::vector<uint8_t> tag;
    std::vector<uint8_t> ciphertext;
};

struct ClientRegister  { uint64_t mac_addr{0}; bool needs_config{false}; };
struct ClientAvailable { bool available{false}; };
struct ClientBattery   { float voltage{0.0f}; };
struct CoverPosition   { float position{0.0f}; float voltage{0.0f}; float current{0.0f}; };
struct CommandAck      { uint32_t ack_msg_id{0}; AckStatus status{AckStatus::ACK_OK}; };

// Hub -> Node
struct LoraClientOperationMessage {
    LoraHeader header;
    enum class Cmd {
        NotSet, Operation, Sysop, ClientConfig, CoverConfig,
        Login, BaseNonce, Encrypted,
        TimeSync, Schedule            // auto-mode (P0)
    } cmd{Cmd::NotSet};
    LoraCoverOperation       operation;
    ClientOperation          sysop{ClientOperation::CMD_STATUS};
    ::proto_sim::ClientConfig clientconfig;
    ::proto_sim::CoverConfig  coverconfig;
    LoginMsg                 login;
    BaseNonceExchange        basenonce;
    EncryptedPayload         encrypted;
    ::proto_sim::TimeSync       timesync;
    ::proto_sim::ScheduleConfig schedule;
};

// Node -> Hub
struct LoraClientResponseMessage {
    LoraHeader header;
    enum class Proto {
        NotSet, Avail, Register, State, Position, Login, Encrypted,
        Ack,
        Beacon                        // auto-mode (P0)
    } proto{Proto::NotSet};
    ClientAvailable  avail;
    ClientRegister   reg;
    ClientBattery    state;
    CoverPosition    position;
    LoginMsg         login;
    EncryptedPayload encrypted;
    CommandAck       ack;
    ::proto_sim::NodeWakeBeacon beacon;
};

// On-air representation used by sim_radio. Stores REAL wire bytes produced by
// blinds.pb-c.c via protobuf-c. The sender serializes; the receiver
// deserializes via wire_codec. Tests inspect transcripts using
// as_op() / as_resp() helpers.
struct AirFrame {
    enum class Dir { HubToNode, NodeToHub } dir;
    std::vector<uint8_t> bytes;
};

} // namespace proto_sim
