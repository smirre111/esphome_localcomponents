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
#include "esphome/components/lora_client/MacWireConfig.h"
#include "esphome/components/lora_client/MacWire.h"

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

// ---------------------------------------------------------------------------
// MAC wire v1 (S8, docs/mac-wire-format.md section 5): the same interface over
// a fixed-offset header. No decode per copy: per-copy work is byte writes at
// the documented offsets (macwire::stampCopy) plus, for an authenticated
// frame, ONE CMAC over the copy's bytes through the frame's own sealer.
//
// The STAMP extension's presence is decided once per burst (setFireStamp with
// the same `stamped` on every copy, which is what sendPacketBurst does): the
// producer packs the frame unstamped and the first stamped copy inserts the
// six bytes before the body. The body (ciphertext) is never touched, and the
// tag covers the raw bytes, so inserting the extension needs no re-seal of
// anything but the tag.
//
// ok() is false for an AUTH = FLEET frame (the GridBeacon): its tag is the
// management unit's and MAC-0 holds no fleet key, so the frame goes out
// verbatim, exactly as packed (one copy, burst 0/1, unstamped).
// ---------------------------------------------------------------------------
class WireBurstFrame
{
public:
    WireBurstFrame(const uint8_t *data, size_t len)
    {
        if (data == nullptr || len > macwire::kMaxFrameBytes)
            return;
        std::memcpy(this->buf_, data, len);
        this->len_ = len;
        size_t off = 0, body = 0;
        this->ok_  = macwire::decode(this->buf_, this->len_, &this->hdr_, &off, &body) &&
                     this->hdr_.auth() != macwire::Auth::Fleet;
        // Start from what the frame says, so a write of one field never
        // clobbers another the caller has not set yet.
        this->idx_     = this->hdr_.burst_idx;
        this->cnt_     = this->hdr_.burst_cnt;
        this->on_mark_ = this->hdr_.onMark();
        this->round_   = this->hdr_.fire_round;
        this->offset_  = this->hdr_.fire_offset_us;
    }

    bool ok() const { return this->ok_; }
    // An authenticated frame: every copy needs its own tag (seal()).
    bool encrypted() const { return this->ok_ && this->hdr_.auth() != macwire::Auth::None; }
    uint32_t msgid() const { return this->ok_ ? this->hdr_.fcnt : 0u; }

    void setBurst(int count, bool on_mark)
    {
        this->cnt_     = static_cast<uint8_t>(count);
        this->on_mark_ = on_mark;
        this->write_();
    }
    void setIndex(int k)
    {
        this->idx_ = static_cast<uint8_t>(k);
        this->write_();
    }
    void setFireStamp(bool stamped, uint32_t round, uint32_t offset_us)
    {
        if (!this->layout_(stamped))
        {
            this->ok_ = false;   // no room for the extension: send nothing indexed
            return;
        }
        this->round_  = round & macwire::kU24Max;   // compared modulo 2^24 (spec 2.3)
        this->offset_ = offset_us;
        this->write_();
    }

    bool seal(mac::CopySealer sealer, uint64_t *binding)
    {
        const size_t n = this->len_ - macwire::kTagBytes;
        size_t off = 0, body = 0;
        mac::MacHdr hdr;
        uint8_t tag[8];
        if (sealer == nullptr || !macwire::decode(this->buf_, this->len_, &this->hdr_, &off, &body))
            return false;
        machdr::fromWireHdr(this->hdr_, &hdr);
        if (!sealer(binding, hdr, this->buf_, n, tag))
            return false;
        std::memcpy(this->buf_ + n, tag, sizeof(tag));
        return true;
    }

    size_t packedSize() const { return this->len_; }
    void   packInto(uint8_t *out) const { std::memcpy(out, this->buf_, this->len_); }

private:
    bool stamped_() const { return (this->buf_[macwire::kOffFlags] & macwire::kFlagStamped) != 0; }

    // Insert or remove the six STAMP bytes at kOffExt (they come first among
    // the extensions), flipping FLAGS.STAMPED with them.
    bool layout_(bool stamped)
    {
        if (stamped == this->stamped_())
            return true;
        uint8_t *ext = this->buf_ + macwire::kOffExt;
        const size_t tail = this->len_ - macwire::kOffExt;
        if (stamped)
        {
            if (this->len_ + macwire::kStampBytes > macwire::kMaxFrameBytes)
                return false;
            std::memmove(ext + macwire::kStampBytes, ext, tail);
            std::memset(ext, 0, macwire::kStampBytes);
            this->len_ += macwire::kStampBytes;
            this->buf_[macwire::kOffFlags] |= macwire::kFlagStamped;
        }
        else
        {
            std::memmove(ext, ext + macwire::kStampBytes, tail - macwire::kStampBytes);
            this->len_ -= macwire::kStampBytes;
            this->buf_[macwire::kOffFlags] &= (uint8_t) ~macwire::kFlagStamped;
        }
        return true;
    }

    void write_()
    {
        if (this->ok_)
            macwire::stampCopy(this->buf_, this->len_, this->idx_, this->cnt_, this->on_mark_,
                               this->stamped_(), this->round_, this->offset_);
    }

    uint8_t          buf_[macwire::kMaxFrameBytes]{};
    size_t           len_{0};
    bool             ok_{false};
    macwire::WireHdr hdr_{};
    uint8_t          idx_{0};
    uint8_t          cnt_{0};
    bool             on_mark_{false};
    uint32_t         round_{0};
    uint32_t         offset_{0};
};

// The burst engine's frame type for this build (spec section 12: a compile-time
// switch, not a runtime dual stack).
#if defined(MAC_WIRE_V1) && MAC_WIRE_V1
using TxFrame = WireBurstFrame;
static constexpr bool kTxFrameInsertsStamp = true;    // +kStampBytes when stamped
#else
using TxFrame = BurstFrame;
static constexpr bool kTxFrameInsertsStamp = false;
#endif

}  // namespace mac0
