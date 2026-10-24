#pragma once
// HubFrames.h -- the hub's INTERNAL frame types (stage S8 W5c).
//
// Until S8 / W5c the listener's producers and handlers used the v0 wire envelopes
// as their in-memory shapes (LoraClientOperationMessage for a command to send,
// LoraClientResponseMessage + LoraHeader + CommandAck for a received frame).
// MAC wire v1 carries a fixed header (MacWire.h) plus ONE body PDU, and the v0
// messages are gone from blinds.proto, so these are plain hub structs, field
// for field the same names, mirroring the node's NodeFrames.h:
//
//   Hdr    the decoded MAC header a handler reads (was LoraHeader)
//   Down   one command to frame: its case and a pointer to its body message
//          (was LoraClientOperationMessage)
//   Up     one received frame: its case and a pointer into the decoded body PDU
//          (was LoraClientResponseMessage)
//   UpAck  the MAC ACK's content: ACKED_FCNT from the header, status + phase
//          from MacAck (was CommandAck)
//   Sealed an AUTH = SESSION frame before it is opened: ciphertext and tag,
//          pointing into the received bytes
//
// The case numbers are the v1 PDU oneof numbers (HubWire maps them case for
// case). Not on the wire, not shared with the node, no behaviour. In a
// namespace because seam/e2e link the node's NodeFrames.h into the same binary.

#include <cstddef>
#include <cstdint>

#include "esphome/components/blindsproto/blinds.pb-c.h"

namespace hubframes
{

struct Hdr
{
    uint32_t destaddress{0};
    uint32_t destsubnet{0};
    uint32_t senderaddress{0};
    uint32_t msgid{0};          // the FCNT
    uint32_t burstindex{0};
    uint32_t burstcount{0};
    bool     onmark{false};
    bool     firestamped{false};
    uint32_t fireround{0};
    uint32_t fireoffsetus{0};
    uint32_t sessionnonce{0};   // uplink: the session-opening NODE_NONCE
};

enum DownCase : int
{
    DL_NOT_SET      = 0,
    DL_OPERATION    = 10,
    DL_SYSOP        = 11,
    DL_CLIENTCONFIG = 12,
    DL_COVERCONFIG  = 13,
    DL_LOGIN        = 14,
    DL_TIMESYNC     = 16,
    DL_SCHEDULE     = 17,
    DL_DRIFTTEST    = 18,
    DL_MACCONTROL   = 20,
    DL_GRIDSYNC     = 21,
    DL_MODETEST     = 22,
    DL_GRIDBEACON   = 23,
    DL_GRIDDEMOTE   = 24,
};

struct Down
{
    DownCase        cmd_case{DL_NOT_SET};
    ClientOperation sysop{(ClientOperation) 0};   // DL_SYSOP only
    union
    {
        void               *body_{nullptr};
        LoraCoverOperation *operation;
        ClientConfig       *clientconfig;
        CoverConfig        *coverconfig;
        LoginMsg           *login;
        TimeSync           *timesync;
        ScheduleConfig     *schedule;
        DriftTest          *drifttest;
        MacControl         *maccontrol;
        GridSync           *gridsync;
        ModeTest           *modetest;
        GridBeacon         *gridbeacon;
        GridDemote         *griddemote;
    };
};

enum UpCase : int
{
    UL_NOT_SET         = 0,
    UL_SEALED          = 9,    // an AUTH = SESSION frame not yet opened
    UL_AVAIL           = 10,
    UL_REGISTER        = 11,
    UL_STATE           = 12,
    UL_POSITION        = 13,
    UL_ACK             = 15,
    UL_BEACON          = 16,
    UL_MACCONTROL      = 20,
    UL_MODETESTREPORT  = 22,
    UL_GRIDSYNCREQUEST = 23,
};

struct UpAck
{
    uint32_t     ack_msg_id{0};   // the acknowledged FCNT (header ACKED_FCNT)
    AckStatus    status{ACK_STATUS__ACK_OK};
    PhaseReport *phase{nullptr};
};

struct Sealed
{
    ProtobufCBinaryData ciphertext{0, nullptr};
    ProtobufCBinaryData tag{0, nullptr};
};

struct Up
{
    Hdr   *header{nullptr};
    UpCase proto_case{UL_NOT_SET};
    union
    {
        void            *body_{nullptr};
        Sealed          *encrypted;
        ClientAvailable *avail;
        ClientRegister  *register_;
        ClientBattery   *state;
        CoverPosition   *position;
        UpAck           *ack;
        NodeWakeBeacon  *beacon;
        MacControl      *maccontrol;
        ModeTestReport  *modetestreport;
        GridSyncRequest *gridsyncrequest;
    };
};

}  // namespace hubframes
