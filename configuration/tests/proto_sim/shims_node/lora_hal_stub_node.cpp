// The node's SX1278 driver, as a recorder. Implements the REAL
// components/lora/include/lora.h rather than a paraphrase of it, so a signature
// that drifts is a build error here instead of a surprise on the roof — the
// same arrangement the hub's shims/lora_hal_stub.cpp uses.
#include <cstring>

// ANGLE BRACKETS, and this is not style. A quoted include searches the
// directory of the FILE doing the including first, and this file lives beside
// the node shims' own 13-line lora.h — all CmdDispatcher ever needed. Quoting
// it here would compile the recorder against that paraphrase and collide with
// its inline definitions, which is the same trap the node_stage comments
// describe. Angle brackets take the REAL driver header from the include path.
#include <lora.h>

#include "lora_node_recorder.h"

namespace loranode {
Recorder &rec() { static Recorder r; return r; }
}

static loranode::Recorder &R() { return loranode::rec(); }

int  lora_init(void)              { R().note("lora_init"); return 1; }
void lora_end()                   { R().note("lora_end"); }

int lora_beginPacket(int implicitHeader) {
    (void) implicitHeader;
    R().note("lora_beginPacket");
    R().staging.clear();
    return 1;   // 1 = FIFO armed; 0 means "already transmitting"
}

int lora_endPacket(bool async) {
    R().note(async ? "lora_endPacket_async" : "lora_endPacket");
    // The synchronous path wraps fire + wait, so it closes a frame the same way
    // lora_tx does. Recorded explicitly rather than by falling through, because
    // which of the two the node used is itself the thing some tests assert.
    R().packets.push_back(R().staging);
    R().tx_us.push_back(esp_timer_get_time());
    R().last_tx_done_us = async ? 0 : esp_timer_get_time();
    R().staging.clear();
    return 1;
}

int64_t lora_lastTxDoneUs(void)   { return R().last_tx_done_us; }
bool    lora_isTransmitting()     { return false; }

int lora_parsePacket(int size) {
    (void) size;
    R().note("lora_parsePacket");
    R().rx_cursor = 0;
    return (int) R().rx_payload.size();
}
int lora_parsePacket(uint8_t irqFlags, int size) {
    (void) irqFlags;
    return lora_parsePacket(size);
}

int   lora_packetRssi(void)       { return -42; }
float lora_packetSnr(void)        { return 10.0f; }
float lora_packetFrequencyError() { return 0.0f; }
void  lora_compensateFrequencyOffset(const float &f) { (void) f; }

size_t lora_write(uint8_t byte) {
    R().staging.push_back(byte);
    return 1;
}
size_t lora_write(const uint8_t *buffer, size_t size) {
    if (buffer != nullptr)
        R().staging.insert(R().staging.end(), buffer, buffer + size);
    return size;
}

int lora_available() {
    return (int) (R().rx_payload.size() - R().rx_cursor);
}
int lora_read() {
    if (R().rx_cursor >= R().rx_payload.size()) return 0;
    return R().rx_payload[R().rx_cursor++];
}
int     lora_peek()               { return lora_available() ? R().rx_payload[R().rx_cursor] : 0; }
void    lora_receive(void)        { R().note("lora_receive"); }
void    lora_receive(int size)    { (void) size; R().note("lora_receive"); }
uint8_t lora_getPayloadLength()   { return (uint8_t) R().rx_payload.size(); }

void lora_idle(void)              { R().note("lora_idle"); }
void lora_sleep(void)             { R().note("lora_sleep"); }
void lora_cad(void)               { R().note("lora_cad"); if (R().on_cad) R().on_cad(); }
void lora_tx()                    { R().note("lora_tx"); }
void lora_rxSingle()              { R().note("lora_rxSingle"); }
void lora_rxContinuous()          { R().note("lora_rxContinuous"); }

void lora_setTxPower(int level, int outputPin) { (void) level; (void) outputPin; R().note("lora_setTxPower"); }
void lora_setTxPower(int level)   { (void) level; R().note("lora_setTxPower"); }
void lora_setFrequency(long f)    { (void) f; R().note("lora_setFrequency"); }
int  lora_getSpreadingFactor()    { return 7; }
void lora_setSpreadingFactor(int sf) { R().sf = sf; R().note("lora_setSpreadingFactor"); }
long lora_getSignalBandwidth()    { return 500000; }
void lora_setSignalBandwidth(long sbw) { R().bw = sbw; R().note("lora_setSignalBandwidth"); }
void lora_setLdoFlag()            { R().note("lora_setLdoFlag"); }
void lora_setCodingRate4(int d)   { R().cr_denom = d; R().note("lora_setCodingRate4"); }
void lora_setPreambleLength(long l) { R().preamble_len = l; R().note("lora_setPreambleLength"); }

void lora_setSymbolTimeout(uint16_t symbols) {
    // Recorded, not discarded: the symbol timeout IS the receive window's
    // width, and Mode B's whole guard-band arithmetic is derived from it.
    R().sym_timeout = symbols;
    R().note("lora_setSymbolTimeout");
}
uint16_t lora_getSymbolTimeout()  { return R().sym_timeout; }

void lora_setSyncWord(int sw)     { R().sync_word = sw; R().note("lora_setSyncWord"); }
void lora_setMaxPayloadLength(const uint8_t len) { (void) len; }
void lora_enableLowDataRateOptimize(bool e) { (void) e; }
void lora_enableTcxo(const bool e){ (void) e; }
void lora_enableCrc(void)         { R().crc_on = true;  R().note("lora_enableCrc"); }
void lora_disableCrc(void)        { R().crc_on = false; R().note("lora_disableCrc"); }
void lora_enableInvertIQ()        {}
void lora_disableInvertIQ()       {}
void lora_setOCP(uint8_t mA)      { (void) mA; }
void lora_setGain(uint8_t gain)   { (void) gain; }

void lora_setInterruptMode(uint8_t pin, uint8_t mode) {
    // Which edge each DIO reports. A window armed with DIO0 still mapped to
    // CADDONE cannot hear a frame, and that is not visible in a call count.
    if (pin < 6) R().dio_mode[pin] = mode;
    R().note("lora_setInterruptMode");
}

uint8_t lora_readInterrupts() {
    // CONSUMED on read. Production clears the flags after acting on them, so a
    // sticky value would turn one injected event into an endless stream and any
    // loop over it would never terminate.
    const uint8_t f = R().next_interrupts;
    R().next_interrupts = 0;
    R().note("lora_readInterrupts");
    return f;
}
void    lora_clearInterrupts(uint8_t f) { (void) f; R().note("lora_clearInterrupts"); }
uint8_t lora_random()             { return 0; }
void    lora_dump_registers(void) {}
DeviceMode lora_getDeviceMode()   { return DeviceMode::Standby; }

void lora_explicitHeaderMode(void)   { R().note("lora_explicitHeaderMode"); }
void lora_implicitHeaderMode(int s)  { (void) s; }
void lora_implicitHeaderMode()       {}
void lora_reset(void)                { R().note("lora_reset"); }
void lora_send_packet(uint8_t *buf, int size) { lora_write(buf, (size_t) size); lora_tx(); }
int  lora_receive_packet(uint8_t *buf, int size) {
    const int n = (int) R().rx_payload.size() < size ? (int) R().rx_payload.size() : size;
    if (buf != nullptr && n > 0) memcpy(buf, R().rx_payload.data(), (size_t) n);
    return n;
}
int  lora_received(void)          { return R().rx_payload.empty() ? 0 : 1; }
void lora_close(void)             { R().note("lora_close"); }
