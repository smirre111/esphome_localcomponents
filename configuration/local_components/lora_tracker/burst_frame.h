#pragma once
// burst_frame.h -- the ONE place MAC-0 (the tracker's burst engine) touches the
// application schema (stage S5, docs/mac-separation-layering-plan.md).
//
// Until the wire stage (S8) the per-copy fields (burstIndex/Count, onMark,
// fireStamped/Round/OffsetUs) live in the protobuf LoraHeader, so restamping a
// copy means decode once, mutate per copy, re-encode per copy. That work is
// confined to this type: sendPacketBurst speaks only "open, stamp, seal, pack".
// At S8 the header becomes fixed-offset bytes and this file shrinks to byte
// writes.
//
// Hub-only. Wire output byte-identical to before S5 (golden frames).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/lora_client/MacHdrCodec.h"
#include "esphome/components/lora_client/MacSap.h"

namespace mac0
{

class BurstFrame
{
public:
    // Decodes `data` once. ok() is false when it is not a frame with a header
    // (the caller then sends the raw bytes, unindexed).
    BurstFrame(const uint8_t *data, size_t len)
        : msg_(lora_client_operation_message__unpack(nullptr, len, data)) {}
    ~BurstFrame()
    {
        if (msg_ != nullptr)
            lora_client_operation_message__free_unpacked(msg_, nullptr);
    }
    BurstFrame(const BurstFrame &) = delete;
    BurstFrame &operator=(const BurstFrame &) = delete;

    bool ok() const { return msg_ != nullptr && msg_->header != nullptr; }
    // Sealed once at pack time: every copy needs its own tag (seal()).
    bool encrypted() const
    {
        return ok() && msg_->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED &&
               msg_->encrypted != nullptr;
    }
    uint32_t msgid() const { return ok() ? msg_->header->msgid : 0u; }

    // Frame-wide fields: the burst's copy count and whether copy 0 sits on the
    // destination's mark.
    void setBurst(int count, bool on_mark)
    {
        msg_->header->burstcount = count;
        msg_->header->onmark     = on_mark ? 1 : 0;
    }
    void setIndex(int k) { msg_->header->burstindex = k; }
    void setFireStamp(bool stamped, uint32_t round, uint32_t offset_us)
    {
        msg_->header->firestamped  = stamped ? 1 : 0;
        msg_->header->fireround    = round;
        msg_->header->fireoffsetus = offset_us;
    }

    // This copy's tag, through the frame's own MAC-2 sealer (S4): the stamped
    // header decoded into a MacHdr, the tag written over the placeholder in
    // place. No key, no client, no heap. False drops the copy.
    bool seal(mac::CopySealer sealer, uint64_t *binding)
    {
        ::EncryptedPayload *enc = msg_->encrypted;
        mac::MacHdr hdr;
        uint8_t tag[8];
        if (sealer == nullptr || !machdr::fromLoraHeader(msg_->header, &hdr) ||
            enc->tag.data == nullptr || enc->tag.len != sizeof(tag))
            return false;
        if (!sealer(binding, hdr, enc->ciphertext.data, enc->ciphertext.len, tag))
            return false;
        std::memcpy(enc->tag.data, tag, sizeof(tag));
        return true;
    }

    size_t packedSize() const { return lora_client_operation_message__get_packed_size(msg_); }
    void   packInto(uint8_t *out) const { lora_client_operation_message__pack(msg_, out); }

private:
    ::LoraClientOperationMessage *msg_;
};

}  // namespace mac0
