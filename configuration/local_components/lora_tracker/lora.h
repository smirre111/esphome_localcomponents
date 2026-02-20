
#ifndef __LORA_H__
#define __LORA_H__

#include <freertos/FreeRTOS.h>
#include "driver/spi_master.h"

#define PA_OUTPUT_RFO_PIN 0
#define PA_OUTPUT_PA_BOOST_PIN 1

// Interrupt related
#define LORA_IRQ_DIO0_RXDONE 0x0
#define LORA_IRQ_DIO0_TXDONE 0x1
#define LORA_IRQ_DIO0_CADDONE 0x2

#define LORA_IRQ_DIO1_RXTIMEOUT 0x0
#define LORA_IRQ_DIO1_FHSSCHCH 0x1
#define LORA_IRQ_DIO1_CADDETECTED 0x2

#define LORA_IRQ_DIO2_FHSSCHCH 0x0

#define LORA_IRQ_DIO3_CADDONE 0x0
#define LORA_IRQ_DIO3_VALIDHEADER 0x1
#define LORA_IRQ_DIO3_CRCERROR 0x2

#define LORA_IRQ_DIO4_CADDETECTED 0x0
#define LORA_IRQ_DIO4_PLLLOCK 0x1

#define LORA_IRQ_DIO5_MODEREADY 0x0
#define LORA_IRQ_DIO5_CLKOUT 0x1

#define LORA_IRQ_FLAG_RX_TIMEOUT 0b10000000
#define LORA_IRQ_FLAG_RX_DONE 0b01000000
#define LORA_IRQ_FLAG_PAYLOAD_CRC_ERROR 0b00100000
#define LORA_IRQ_FLAG_VALID_HEADER 0b00010000
#define LORA_IRQ_FLAG_TX_DONE 0b00001000
#define LORA_IRQ_FLAG_CAD_DONE 0b00000100
#define LORA_IRQ_FLAG_FHSS_CHANGE_CH 0b00000010
#define LORA_IRQ_FLAG_CAD_DETECTED 0b00000001

// Device modes
#define LORA_MODE_LONG_RANGE_MODE 0x80
#define LORA_MODE_SLEEP 0x00
#define LORA_MODE_STDBY 0x01
#define LORA_MODE_FSTX 0x02
#define LORA_MODE_TX 0x03
#define LORA_MODE_FSRX 0x04
#define LORA_MODE_RX_CONTINUOUS 0x05
#define LORA_MODE_RX_SINGLE 0x06
#define LORA_MODE_CAD 0x07


struct DeviceMode 
{
  static const int Sleep = LORA_MODE_SLEEP;
  static const int Standby = LORA_MODE_STDBY;
  static const int FrequencySynthesisTx = LORA_MODE_FSTX;
  static const int Transmit = LORA_MODE_TX;
  static const int FrequencySynthesisRx = LORA_MODE_FSRX;
  static const int ReceiveContinous = LORA_MODE_RX_CONTINUOUS;
  static const int ReceiveSingle = LORA_MODE_RX_SINGLE;
  static const int ChannelActivityDetection = LORA_MODE_CAD;
};

int lora_init(void);
void lora_end();
int lora_beginPacket(int implicitHeader = false);
int lora_endPacket(bool async = false);
bool lora_isTransmitting();
int lora_parsePacket(int size);
int lora_parsePacket(uint8_t irqFlags, int size);
int lora_packetRssi(void);
float lora_packetSnr(void);
float lora_packetFrequencyError();
void lora_compensateFrequencyOffset(const float &fError);
size_t lora_write(uint8_t byte);
size_t lora_write(const uint8_t *buffer, size_t size);
int lora_available();
int lora_read();
int lora_peek();
void lora_receive(void);
void lora_receive(int size);
uint8_t lora_getPayloadLength();

void lora_idle(void);
void lora_sleep(void);
void lora_cad(void);
void lora_tx();
void lora_rxSingle();
void lora_rxContinuous();

void lora_setTxPower(int level, int outputPin);
void lora_setTxPower(int level);
void lora_setFrequency(long frequency);
int lora_getSpreadingFactor();
void lora_setSpreadingFactor(int sf);

long lora_getSignalBandwidth();
void lora_setSignalBandwidth(long sbw);
void lora_setLdoFlag();

void lora_setCodingRate4(int denominator);
void lora_setPreambleLength(long length);
void lora_setSymbolTimeout(uint16_t symbols);
uint16_t lora_getSymbolTimeout();
void lora_setSyncWord(int sw);
void lora_setMaxPayloadLength(const uint8_t payloadLength);
void lora_enableLowDataRateOptimize(bool enabled);
void lora_enableTcxo(const bool enabled);
void lora_enableCrc(void);
void lora_disableCrc(void);
void lora_enableInvertIQ();
void lora_disableInvertIQ();
void lora_setOCP(uint8_t mA);
void lora_setGain(uint8_t gain);
void lora_setInterruptMode(uint8_t pin, uint8_t mode); // pin: [DIO]0..5; mode: see LORA_IRQ_DIO*
uint8_t lora_readInterrupts();
void lora_clearInterrupts(uint8_t irqFlags);
uint8_t lora_random();
void lora_dump_registers(void);
uint8_t lora_getDeviceMode();

void lora_explicitHeaderMode(void);
void lora_implicitHeaderMode(int size);
void lora_implicitHeaderMode();
void lora_reset(void);
void lora_send_packet(uint8_t *buf, int size);
int lora_receive_packet(uint8_t *buf, int size);
int lora_received(void);
void lora_close(void);

#endif
