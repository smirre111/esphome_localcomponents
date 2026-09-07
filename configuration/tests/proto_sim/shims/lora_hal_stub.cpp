#include <cstring>
#include "lora.h"
#include "lora_hal_recorder.h"

namespace lorahal {
Recorder& rec() { static Recorder r; return r; }
}

int lora_init() {
    lorahal::rec().note("lora_init");
    return 1;  // production returns 1 on success
}

void lora_end() {
    lorahal::rec().note("lora_end");
}

int lora_beginPacket(int implicitHeader) {
    (void) implicitHeader;
    lorahal::rec().note("lora_beginPacket");
    // 1 = FIFO armed. 0 means "already transmitting", which makes the tracker
    // log a warning and push the payload into a busy radio — a state the stub
    // must not enter by accident, or every TX test silently exercises the
    // error path instead of the one under test.
    return 1;
}

int lora_waitTxDone() {
    lorahal::rec().note("lora_waitTxDone");
    return 1;  // the fake radio always finishes; 0 is the timeout path
}

int64_t lora_lastTxDoneUs() {
    return lorahal::rec().txdone_us;
}

int lora_endPacket(bool async) {
    (void) async;
    lorahal::rec().note("lora_endPacket");
    return 1;  // 0 is production's TX-timeout signal
}

bool lora_isTransmitting() {
    lorahal::rec().note("lora_isTransmitting");
    return 0;
}

int lora_parsePacket(int size) {
    (void) size;
    lorahal::rec().note("lora_parsePacket");
    return 0;
}

int lora_parsePacket(uint8_t irqFlags, int size) {
    (void) irqFlags;
    (void) size;
    lorahal::rec().note("lora_parsePacket");
    return 0;
}

int lora_packetRssi() {
    lorahal::rec().note("lora_packetRssi");
    return 0;
}

float lora_packetSnr() {
    lorahal::rec().note("lora_packetSnr");
    return 0;
}

float lora_packetFrequencyError() {
    lorahal::rec().note("lora_packetFrequencyError");
    return 0;
}

void lora_compensateFrequencyOffset(const float &fError) {
    (void) &fError;
    lorahal::rec().note("lora_compensateFrequencyOffset");
}

size_t lora_write(uint8_t byte) {
    (void) byte;
    lorahal::rec().note("lora_write");
    return 0;
}

size_t lora_write(const uint8_t *buffer, size_t size) {
    // The payload path: record the bytes, which is what burst
    // structure and per-frame TX policy are actually made of.
    lorahal::rec().note("lora_write");
    lorahal::rec().wrote(buffer, size);
    return size;
}

int lora_available() {
    lorahal::rec().note("lora_available");
    return 0;
}

int lora_read() {
    lorahal::rec().note("lora_read");
    return 0;
}

int lora_peek() {
    lorahal::rec().note("lora_peek");
    return 0;
}

void lora_receive() {
    lorahal::rec().note("lora_receive");
}

void lora_receive(int size) {
    (void) size;
    lorahal::rec().note("lora_receive");
}

uint8_t lora_getPayloadLength() {
    lorahal::rec().note("lora_getPayloadLength");
    return 0;
}

void lora_idle() {
    lorahal::rec().note("lora_idle");
}

void lora_sleep() {
    lorahal::rec().note("lora_sleep");
}

void lora_cad() {
    lorahal::rec().note("lora_cad");
}

void lora_tx() {
    lorahal::rec().note("lora_tx");
}

void lora_rxSingle() {
    lorahal::rec().note("lora_rxSingle");
}

void lora_rxContinuous() {
    lorahal::rec().note("lora_rxContinuous");
}

void lora_setTxPower(int level, int outputPin) {
    (void) level;
    (void) outputPin;
    lorahal::rec().note("lora_setTxPower");
}

void lora_setTxPower(int level) {
    (void) level;
    lorahal::rec().note("lora_setTxPower");
}

void lora_setFrequency(long frequency) {
    (void) frequency;
    lorahal::rec().note("lora_setFrequency");
}

int lora_getSpreadingFactor() {
    lorahal::rec().note("lora_getSpreadingFactor");
    return 0;
}

void lora_setSpreadingFactor(int sf) {
    (void) sf;
    lorahal::rec().note("lora_setSpreadingFactor");
}

long lora_getSignalBandwidth() {
    lorahal::rec().note("lora_getSignalBandwidth");
    return 0;
}

void lora_setSignalBandwidth(long sbw) {
    (void) sbw;
    lorahal::rec().note("lora_setSignalBandwidth");
}

void lora_setLdoFlag() {
    lorahal::rec().note("lora_setLdoFlag");
}

void lora_setCodingRate4(int denominator) {
    (void) denominator;
    lorahal::rec().note("lora_setCodingRate4");
}

void lora_setPreambleLength(long length) {
    (void) length;
    lorahal::rec().note("lora_setPreambleLength");
}

void lora_setSymbolTimeout(uint16_t symbols) {
    (void) symbols;
    lorahal::rec().note("lora_setSymbolTimeout");
}

uint16_t lora_getSymbolTimeout() {
    lorahal::rec().note("lora_getSymbolTimeout");
    return 0;
}

void lora_setSyncWord(int sw) {
    (void) sw;
    lorahal::rec().note("lora_setSyncWord");
}

void lora_setMaxPayloadLength(const uint8_t payloadLength) {
    (void) payloadLength;
    lorahal::rec().note("lora_setMaxPayloadLength");
}

void lora_enableLowDataRateOptimize(bool enabled) {
    (void) enabled;
    lorahal::rec().note("lora_enableLowDataRateOptimize");
}

void lora_enableTcxo(const bool enabled) {
    (void) enabled;
    lorahal::rec().note("lora_enableTcxo");
}

void lora_enableCrc() {
    lorahal::rec().note("lora_enableCrc");
}

void lora_disableCrc() {
    lorahal::rec().note("lora_disableCrc");
}

void lora_enableInvertIQ() {
    lorahal::rec().note("lora_enableInvertIQ");
}

void lora_disableInvertIQ() {
    lorahal::rec().note("lora_disableInvertIQ");
}

void lora_setOCP(uint8_t mA) {
    (void) mA;
    lorahal::rec().note("lora_setOCP");
}

void lora_setGain(uint8_t gain) {
    (void) gain;
    lorahal::rec().note("lora_setGain");
}

void lora_setInterruptMode(uint8_t pin, uint8_t mode) {
    (void) pin;
    (void) mode;
    lorahal::rec().note("lora_setInterruptMode");
}

uint8_t lora_readInterrupts() {
    lorahal::rec().note("lora_readInterrupts");
    return 0;
}

void lora_clearInterrupts(uint8_t irqFlags) {
    (void) irqFlags;
    lorahal::rec().note("lora_clearInterrupts");
}

uint8_t lora_random() {
    lorahal::rec().note("lora_random");
    return 0;
}

void lora_dump_registers() {
    lorahal::rec().note("lora_dump_registers");
}

uint8_t lora_getDeviceMode() {
    lorahal::rec().note("lora_getDeviceMode");
    return 0;
}

void lora_explicitHeaderMode() {
    lorahal::rec().note("lora_explicitHeaderMode");
}

void lora_implicitHeaderMode(int size) {
    (void) size;
    lorahal::rec().note("lora_implicitHeaderMode");
}

void lora_implicitHeaderMode() {
    lorahal::rec().note("lora_implicitHeaderMode");
}

void lora_reset() {
    lorahal::rec().note("lora_reset");
}

void lora_send_packet(uint8_t *buf, int size) {
    (void) buf;
    (void) size;
    lorahal::rec().note("lora_send_packet");
}

int lora_receive_packet(uint8_t *buf, int size) {
    lorahal::rec().note("lora_receive_packet");
    auto& in = lorahal::rec().inbox;
    if (in.empty()) return 0;

    const auto frame = in.front();
    in.erase(in.begin());
    const int n = (int) (frame.size() < (size_t) size ? frame.size() : (size_t) size);
    if (buf && n > 0) memcpy(buf, frame.data(), (size_t) n);
    return n;
}

int lora_received() {
    lorahal::rec().note("lora_received");
    return 0;
}

void lora_close() {
    lorahal::rec().note("lora_close");
}
