
#include <driver/rtc_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_app_desc.h>
#include <esp_random.h>
#include <esp_task_wdt.h>

#include "common.h"
#include "CmdDispatcher.h"
#include <esp_private/esp_clk.h>   // esp_clk_cpu_freq(), for the ModeTest report
#include "FrameCrypto.h"
#include "MotorPolicy.h"   // keptTravelDurationS: a CoverConfig zero is "unset", not a duration
#include "utilities.h"
#include "blinds.pb-c.h"
#include "comm_utils.h"
#include <esp_system.h>
// #include "esp_tls.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"
// F-25: <map> removed — std::map frame counters replaced by PeerCounter array.
#include <cstring>
#include <inttypes.h>
#include <esp_attr.h>
#include <sys/time.h>
#include <time.h>
#include "NodeClock.h"   // MAC-0 timebase: esp_timer, re-anchored to the crystal across sleep
#include "BeaconTicks.h" // Mode C wake clock: RTC ticks at a beacon's T0

extern void spawnTaskBatteryMonitor();
// F-39: spawnTaskBatteryMonitorWithUDP / spawnTaskMotorCurrentMonitorWithUDP
//       removed 2026-05-25 — dead code, tasks were never spawned in production.
extern void spawnTaskMotorCurrentMonitor();
// F-29: rx_message_counter / tx_message_counter removed — message IDs are now
//       always re-negotiated via LoginMsg on every boot.
extern uint8_t trigger_ota;

extern TaskHandle_t xHandleLoraPolling;
extern TaskHandle_t xHandleMotor;

static const char *TAG = "CmdDispatcher";

// Last-known-good battery voltage.
//
// This was a plain CmdDispatcher member initialised to 0.0f, so every
// deep-sleep wake reset it — and the wake beacon goes out within a few seconds,
// while the next battery sample is up to batteryInterval (900 s) away. Every
// beacon therefore reported v=0.00 for a perfectly healthy battery, and the hub
// published that as 0.0 V. During this work that reading was briefly mistaken
// for a failing supply.
//
// RTC_DATA_ATTR makes the "LKG cache" its own comment described actually
// survive the one event that clears it. A true power-on still starts at 0,
// which is correct: there is then genuinely no last-known-good value, and the
// beacon's zero honestly means "unknown".
static RTC_DATA_ATTR float s_lastBatteryVoltage = 0.0f;

static constexpr const char *kLoRaAesGcmKey = "LoRaKey1";
static constexpr size_t kAesGcmKeyBytes = 16;
static constexpr size_t kAesGcmIvBytes = 12;
static constexpr size_t kAesGcmTagBytes = 8; // truncated AES-GCM tag (slim on-air)
// PSA algorithm carrying the shortened tag length — used for the key policy and
// every encrypt/decrypt so both sides agree on the 8-byte tag.
#define LORA_GCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, kAesGcmTagBytes)

// Direction separation for the GCM nonce.  Uplink (node->hub) and downlink
// (hub->node) share the same per-peer base nonce and both derive the IV as
// base_nonce || counter, so without a direction bit an uplink frame and a
// downlink frame with the same msgid would reuse an IV — fatal for AES-GCM.
// Setting the MSB of the 64-bit counter for downlink frames separates the two
// IV spaces.  msgid is a uint32 counter and never approaches 2^63, so the bit
// is always free.  Uplink leaves it clear, keeping uplink IVs byte-identical to
// the pre-change format (backward compatible).
static constexpr uint64_t kDownlinkNonceFlag = framecrypto::kDownlinkFlag;

// F-5: NVS namespace/key for persistent counter + base-nonce state.
static constexpr const char *kPersistNvsNamespace = "loractr";
static constexpr const char *kPersistNvsKey       = "peerstate";

// ---------------------------------------------------------------------------
// P1: node wall clock.
//
// The node has NO clock source of its own — no SNTP, no RTC battery — so wall
// time is seeded entirely by the hub's TimeSync.  Two properties matter:
//
//   * The ESP32 keeps system time running ACROSS deep sleep (the RTC timer is
//     the time base), so the clock itself survives a wake.  Plain RAM does not,
//     which is why the validity flag and the offset live in RTC_DATA_ATTR.
//   * The RTC runs off the external 32.768 kHz crystal on this board
//     (CONFIG_RTC_CLK_SRC_EXT_CRYS, ~±20 ppm ≈ 2 s/day), which is what makes
//     uncapped deep sleep between scheduled events viable once seeded.
//
// Scheduling itself is NOT implemented yet — this only establishes and reports
// the clock, so drift can be measured on real hardware before anything depends
// on it.
// ---------------------------------------------------------------------------
static RTC_DATA_ATTR bool     s_clock_valid  = false;
static RTC_DATA_ATTR int32_t  s_utc_offset_s = 0;
static RTC_DATA_ATTR uint64_t s_dst_next     = 0;

// Mode C, MAC-0: the wake clock (BeaconTicks.h, hub WakeClockFit.h). The RTC
// tick count at the last beacon's T0 and that beacon's msgid, reported in the
// NEXT beacon so the hub can pair it with its own receive stamp. RTC_DATA_ATTR:
// survives deep sleep and is zeroed at power-on — exactly when the RTC counter
// itself restarts, so a stale count can never outlive the counter it came from.
static RTC_DATA_ATTR uint32_t s_beacon_msgid      = 0;
static RTC_DATA_ATTR uint64_t s_beacon_t0_ticks   = 0;
// The previous wake's receive funnel, saved at deep-sleep entry: Mode C's FER.
static RTC_DATA_ATTR uint32_t s_prev_wake_windows  = 0;
static RTC_DATA_ATTR uint32_t s_prev_wake_hits     = 0;
static RTC_DATA_ATTR uint32_t s_prev_wake_detected = 0;
static RTC_DATA_ATTR uint32_t s_prev_wake_crcvalid = 0;
// The deep sleep before this wake, as wanted and as handed to ESP-IDF after the
// crystal correction (SleepClockCorrection.h deepSleepRequestUs). Mode C's pass
// line is computed on the hub from these and its wake-clock fit.
static RTC_DATA_ATTR uint64_t s_prev_sleep_requested_us = 0;
static RTC_DATA_ATTR uint64_t s_prev_sleep_applied_us   = 0;

// What the HUB was last told about our Mode B status, and when. RTC_DATA_ATTR
// for the same reason as the clock and the battery cache above: a deep-sleep
// wake must not forget it, a power-on must.
//
// As plain members these were reset by every wake, so the throttle saw
// s_since = 0xFFFFFFFF and the edge test saw 0xFF — every wake was a
// first-ever announcement. The node announced, then sent its check-in beacon
// anyway, spending two uplinks to say one thing. Measured on fw 1.1.2 and
// again on 1.1.3: 2 MODE_CHANGED + 2 TIMER_CHECKIN across two Mode C wakes.
static RTC_DATA_ATTR uint8_t s_last_reason_announced = 0xFF;
static RTC_DATA_ATTR int64_t s_last_mode_announce_us = 0;

uint8_t CmdDispatcher::lastReasonAnnounced() { return s_last_reason_announced; }

void CmdDispatcher::resetModeAnnounceState()
{
  s_last_reason_announced = 0xFF;
  s_last_mode_announce_us = 0;
}

// A deep-sleep wake: plain RAM is gone, RTC_DATA_ATTR survives.
//
// Only the PENDING INTENT is cleared here. s_last_reason_announced and
// s_last_mode_announce_us are deliberately left alone — that is the whole
// property, and a test that clears them is modelling a power-on instead.
void CmdDispatcher::simulateWakeForTest()
{
  this->mode_announce_pending_ = false;
  this->last_reason_logged_    = 0xFF;   // the console's field, also plain RAM
}

bool     CmdDispatcher::isClockValid()  { return s_clock_valid; }
int32_t  CmdDispatcher::getUtcOffset()  { return s_utc_offset_s; }
uint64_t CmdDispatcher::getDstNext()    { return s_dst_next; }

// Format an epoch as LOCAL wall time using the hub-supplied offset.  Avoids
// setenv("TZ")/tzset() and the whole timezone database: entries are expressed
// in local minutes-of-day and the hub already resolved DST for us.
void CmdDispatcher::formatLocalTime(uint64_t epoch, char *out, size_t out_len)
{
  const time_t shifted = static_cast<time_t>(epoch) + s_utc_offset_s;
  struct tm tm_local;
  gmtime_r(&shifted, &tm_local);
  strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tm_local);
}

static bool build_header_aad(const LoraHeader *header, uint8_t *aad_out, size_t *aad_len)
{
    if (!header || !aad_out || !aad_len)
    {
        return false;
    }

    // Layout lives in FrameCrypto.h, where it is pinned field by field and
    // tested on the host. Every AEAD failure looks the same from outside
    // (psa_aead_decrypt: -149) regardless of cause, so the bytes are worth
    // verifying somewhere they can be seen.
    framecrypto::buildAad(header->destaddress, header->destsubnet,
                          header->senderaddress, header->msgid, aad_out);
    *aad_len = framecrypto::kAadBytes;
    return true;
}

// ---------------------------------------------------------------------------
// F-22: PSA key lifecycle — import once, reuse on every encrypt/decrypt.
// ---------------------------------------------------------------------------
bool CmdDispatcher::init_psa_key()
{
  if (aes_gcm_key_id_ != PSA_KEY_ID_NULL)
  {
    return true; // already imported
  }

  uint8_t key_material[kAesGcmKeyBytes];
  if (!derive_aes_gcm_key(key_material))
  {
    return false;
  }

  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  // Allow both encrypt and decrypt with the same key slot.
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attrs, LORA_GCM_ALG);
  psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attrs, kAesGcmKeyBytes * 8);
  // Volatile lifetime: key is auto-destroyed when the device resets.
  psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);

  psa_status_t status = psa_import_key(&attrs, key_material, kAesGcmKeyBytes, &aes_gcm_key_id_);
  memset(key_material, 0, sizeof(key_material)); // zeroise key material from stack
  if (status != PSA_SUCCESS)
  {
    ESP_LOGE(TAG, "init_psa_key: psa_import_key failed: %d", static_cast<int>(status));
    aes_gcm_key_id_ = PSA_KEY_ID_NULL;
    return false;
  }
  ESP_LOGI(TAG, "AES-GCM key imported into PSA slot (id=%u)", (unsigned)aes_gcm_key_id_);
  return true;
}

// ---------------------------------------------------------------------------
// F-25: Fixed-array peer counter helpers (replace std::map).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------

const float adcVoltageDivider = (13000.0 + 100000.0) / 13000.0;
const float adcRatio = 1.85 / 4096.0 * adcVoltageDivider; // 1100mV / maxAdcValue + 6dB

// BlndState blindsStatePbToState(const ha_blinds_state_t &state)
//   switch (state)
//   case BLINDS_MQTT_OPEN:
//   case BLINDS_MQTT_OPENING:
//   case BLINDS_MQTT_CLOSING:
//   case BLINDS_MQTT_CLOSED:
//   case BLINDS_MQTT_UNAVAILABLE:
//   case BLINDS_MQTT_UNKNOWN:
//   default:

blinds_syscmd_base_t blindsOpPbToCmd(const CovOperation &op)
{
  switch (op)
  {

  case COV_OPERATION__CMD_OPEN:
    return BlindsOpCmd::SYSCMD_OPEN;
    break;
  case COV_OPERATION__CMD_CLOSE:
    return BlindsOpCmd::SYSCMD_CLOSE;
    break;
  case COV_OPERATION__CMD_STOP:
    return BlindsOpCmd::SYSCMD_STOP;
    break;
  default:
    return BlindsOpCmd::SYSCMD_STOP;
  }
}

blinds_syscmd_base_t blindsSysPbToCmd(const ClientOperation &op)
{
  switch (op)
  {
  case CLIENT_OPERATION__CMD_ENABLE_WIFI:
    return BlindsSysCmd::SYSCMD_ENABLE_WIFI;
    break;
  case CLIENT_OPERATION__CMD_DISABLE_WIFI:
    return BlindsSysCmd::SYSCMD_DISABLE_WIFI;
    break;
  case CLIENT_OPERATION__CMD_OTA:
    return BlindsSysCmd::SYSCMD_OTA;
    break;
  case CLIENT_OPERATION__CMD_STATUS:
    return BlindsSysCmd::SYSCMD_STATUS;
    break;
  case CLIENT_OPERATION__CMD_SLEEP:
    return BlindsSysCmd::SYSCMD_SLEEP;
    break;
  default:
    return 0;
  }
}

CmdDispatcher::CmdDispatcher(

    MotorCtrl *motCtrl,
    SystemCtrl *sysCtrl,
    LoraInterface *loraIf,

    portMUX_TYPE &motorMux,
    portMUX_TYPE &buttonMux)

    : motCtrl(motCtrl),
      sysCtrl(sysCtrl),
      loraIf(loraIf),

      rxPacket(),
      txPacket(),
      destAddress(0xFF),
      destSubnet(0x00),
      motorMux(motorMux),
      buttonMux(buttonMux),
      rx_free_buffer_queue(nullptr),
      rx_data_queue(nullptr),
      rx_pool_mutex(nullptr)

{

  rxCmdQueueNew = xQueueCreate(5, sizeof(blinds_syscmd_base_t));
  // F-4: txCmdQueueNew carries tx_command_t so the ACK msgid travels with the
  // command instead of via a shared member the dispatcher task could clobber.
  txCmdQueueNew = xQueueCreate(5, sizeof(tx_command_t));
  sysCmdQueueNew = xQueueCreate(2, sizeof(blinds_syscmd_base_t));
  if (rxCmdQueueNew == NULL)
  {
    ESP_LOGE(TAG, "Could not create RX queue");
  }
  if (txCmdQueueNew == NULL)
  {
    ESP_LOGE(TAG, "Could not create TX queue");
  }
  if (sysCmdQueueNew == NULL)
  {
    ESP_LOGE(TAG, "Could not create SYS queue");
  }
  // batteryVoltageValQueue removed — replaced by s_lastBatteryVoltage LKG cache.
  ESP_ERROR_CHECK(init_memory_pool());

  // F-22: Import the AES-GCM key once at startup instead of on every
  //       encrypt/decrypt call.  psa_crypto_init() must have been called
  //       before this constructor runs (done in app_main).
  if (!init_psa_key())
  {
    ESP_LOGE(TAG, "Failed to pre-import AES-GCM key — crypto will fail");
  }
}

void CmdDispatcher::setAddress(uint8_t cfgAddress, uint8_t cfgSubnet)
{
  sysCtrl->setAddress(cfgAddress, cfgSubnet);
}

// void CmdDispatcher::setConfig(Config *cfg)

// The BODY of the transmit task, as a function.
//
// It used to sit inline in processTxCommand, inside a `for(;;)` blocking on
// xQueueReceive(portMAX_DELAY) — so the host suite could not run it at all.
// Section 11b records one consequence: the two lines that assign the phase
// report into the outgoing message were "not covered end to end". The hole is
// larger than that note implies, because this is where EVERY uplink this node
// sends is built, stamped with its msgid and packed.
//
// Splitting the body out changes no behaviour: the task still loops, still
// blocks, and still calls this exactly once per command. What it buys is that a
// test can drive the real uplink path instead of a hand-written mirror of it.
void CmdDispatcher::serviceTxCommand(const tx_command_t &txcmd_in)
{
  tx_command_t txcmd = txcmd_in;

  ClientBattery state = CLIENT_BATTERY__INIT;
  ClientAvailable avail = CLIENT_AVAILABLE__INIT;
  ClientRegister reg = CLIENT_REGISTER__INIT;
  CoverPosition pos = COVER_POSITION__INIT;
  CommandAck ack = COMMAND_ACK__INIT;
  NodeWakeBeacon beacon = NODE_WAKE_BEACON__INIT;
  // Storage for the phase reports below. Declared here with the rest so its
  // lifetime covers pack_response_message() at the bottom of the loop — a
  // block-scoped one would dangle by the time the message is packed.
  PhaseReport beacon_phase = PHASE_REPORT__INIT;
  PhaseReport ack_phase    = PHASE_REPORT__INIT;
  GridSyncRequest gsreq   = GRID_SYNC_REQUEST__INIT;

  LoraClientResponseMessage message = LORA_CLIENT_RESPONSE_MESSAGE__INIT;

  const blinds_syscmd_base_t cmd = txcmd.cmd;

    ESP_LOGI(TAG, "TX Command queue processing");

    switch (cmd)
    {
    case BlindsStatusCmd::SYSCMD_BATTERY:
    {
      ESP_LOGI(TAG, "Sending BATTERY response");
      // Read from the LKG cache — written by setBatteryVoltage() whenever
      // taskBatteryMonitor completes a measurement.  No blocking wait needed.

      state.voltage = s_lastBatteryVoltage;

      message.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_STATE;
      message.state = &state;
    }
    break;

    case BlindsStatusCmd::SYSCMD_REGISTER:
    {
      ESP_LOGI(TAG, "Sending REGISTER response");
      // When we need to send REGISTER status, reset message ID counter
      this->session_.resetCounters();
      // Drop any base nonce restored from NVS (F-5).  We are restarting the
      // register->login handshake, so the old nonce is stale: keeping it would
      // cause pack_response_message() to encrypt follow-up messages with a
      // nonce the hub has already rotated.  CMD_LOGIN reinstalls a fresh one.
      this->clear_base_nonce(this->destAddress);

      uint8_t mac_arr[6];
      ESP_ERROR_CHECK(esp_read_mac(mac_arr, ESP_MAC_EFUSE_FACTORY));
      uint64_t mac = 0;
      mac |= mac_arr[0];
      mac <<= 8;
      mac |= mac_arr[1];
      mac <<= 8;
      mac |= mac_arr[2];
      mac <<= 8;
      mac |= mac_arr[3];
      mac <<= 8;
      mac |= mac_arr[4];
      mac <<= 8;
      mac |= mac_arr[5];

      reg.mac_addr = mac;
      // Tell the hub whether we still need configuration.  Use the PERSISTED
      // config address (0 = never provisioned) as the signal — getRegistered()
      // is a RAM-only flag that resets every boot, which would make every wake
      // look unprovisioned and defeat the hub's config-skip optimisation.
      reg.needs_config = (this->sysCtrl->getConfigAddress() == 0);

      message.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER;
      message.register_ = &reg;
    }
    break;
    case BlindsStatusCmd::SYSCMD_AVAILABLE:
    {

      avail.available = true;

      message.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_AVAIL;
      message.avail = &avail;
    }
    break;
    case BlindsStatusCmd::SYSCMD_BEACON:
    {
      // P2: one frame carrying everything the hub needs on a wake — why we
      // woke, what our clock reads (so the hub can measure our drift without
      // a serial cable), whether it may skip the login handshake, and the
      // telemetry the periodic battery timer would otherwise provide.
      const State st = motCtrl->getState();
      struct timeval now;
      gettimeofday(&now, NULL);

      beacon.reason         = static_cast<WakeReason>(txcmd.arg);
      beacon.schedversion   = sysCtrl->getSchedVersion();
      beacon.nodeepoch      = s_clock_valid ? static_cast<uint64_t>(now.tv_sec) : 0;
      beacon.mode           = sysCtrl->getAutoMode() ? NODE_MODE__MODE_AUTO
                                                     : NODE_MODE__MODE_INTERACTIVE;
      beacon.voltage        = s_lastBatteryVoltage;
      beacon.position       = st.position_;
      beacon.awakewindow_ms = sysCtrl->getPostEventWindow() * 1000u;
      beacon.nexteventepoch = this->computeNextEvent();
      // Session resume (I2): tell the hub we still hold a usable AEAD session,
      // so it can skip the login handshake and save ~4 s of awake radio.  Only
      // claimable when the restored state is valid AND we actually have a base
      // nonce for this peer — otherwise our first encrypted reply would fail
      // the hub's tag check and cost far more than the handshake saved.
      uint32_t unused_nonce = 0;
      beacon.sessionresume  = this->session_.hasValidState() &&
                              this->get_base_nonce(this->destAddress, unused_nonce);
      beacon.clockvalid     = s_clock_valid;
      beacon.fwversion      = CmdDispatcher::firmwareVersion();
      // Why the node BOOTED, as distinct from why it woke. A field node with
      // no serial cable is otherwise undiagnosable: a cold boot that lost RTC
      // RAM, a brownout and a clean OTA restart are indistinguishable from
      // the hub, and all three have been guessed at rather than known.
      beacon.resetreason    = (uint32_t) esp_reset_reason();
      this->fillPhaseReport(beacon_phase);
      beacon.phase          = &beacon_phase;
      // Mode C wake clock and funnel, both about the PREVIOUS beacon / wake.
      beacon.prevbeaconmsgid   = s_beacon_msgid;
      beacon.prevbeacont0ticks = s_beacon_t0_ticks;
      beacon.rtcperiodq19      = nodeclock::periodQ19();
      beacon.prevwakewindows   = s_prev_wake_windows;
      beacon.prevwakehits      = s_prev_wake_hits;
      beacon.prevwakedetected  = s_prev_wake_detected;
      beacon.prevwakecrcvalid  = s_prev_wake_crcvalid;
      beacon.prevsleeprequestedus = s_prev_sleep_requested_us;
      beacon.prevsleepappliedus   = s_prev_sleep_applied_us;

      ESP_LOGI(TAG, "Sending BEACON (reason=%d resume=%d clock=%d v=%.2f pos=%.2f)",
               (int) beacon.reason, (int) beacon.sessionresume,
               (int) beacon.clockvalid, beacon.voltage, beacon.position);

      message.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_BEACON;
      message.beacon = &beacon;

      // Refresh the battery cache if it holds nothing.
      //
      // s_lastBatteryVoltage is RTC_DATA_ATTR, so it survives deep sleep and
      // an auto-mode node normally carries a reading between wakes. But a
      // POWERON clears RTC RAM, and the only things that MEASURE are a 15 min
      // timer in taskBatteryMonitor and the end of a motor move. An auto-mode
      // node is awake ~20 s per hour, so that timer can never expire -- the
      // cache stays 0.00 V forever and every beacon reports it.
      //
      // That is a design assumption from interactive mode (always awake, so
      // the timer always fires) which fails silently in automatic mode.
      // Observed: every beacon for a full night reporting v=0.00.
      //
      // Queued, not awaited: the measurement needs the supply to stabilise,
      // so it lands in the cache for the NEXT beacon. Self-limiting -- once
      // there is a reading this stops firing.
      if (s_lastBatteryVoltage == 0.0f)
      {
        ESP_LOGI(TAG, "Battery cache empty — queueing a measurement for the next beacon");
        this->measureAndSendBatteryVoltage();
      }
    }
    break;
    case BlindsStatusCmd::SYSCMD_POSITION:
    {
      State st = motCtrl->getState();

      pos.position = st.position_;
      // Battery voltage: LKG cache — no blocking wait, always a valid
      // (possibly slightly stale) value from the last measurement.
      pos.voltage  = s_lastBatteryVoltage;
      // Motor current: LKG ADC value — updated by fsmProcess() on every
      // current-sensing tick while the motor is running.
      // Motor current in AMPS, converted by taskMotorCurrentSensing from the
      // VNH5019 CS reading on every current-sensing tick while the motor runs,
      // and 0 while it is idle. It was raw ADC counts here until the
      // battery-voltage branch was merged; the hub's sensor declares amps to
      // match, and the two halves only make sense together.
      //
      // The FSM still consumes RAW counts for its current-sense endstop, which
      // is deliberate: that threshold was tuned against counts and changing its
      // units would change stop behaviour.
      pos.current  = motCtrl->getLastMotorCurrentAmps();
      // Battery-runtime compensation telemetry -- hub-visibility only, 0/0
      // (proto3 "absent") unless a full move just ended via the endstop.
      // See docs/battery-runtime-compensation-proposal.md (hub repo).
      pos.actualruns  = motCtrl->getLastActualRunS();
      pos.nominalruns = motCtrl->getLastNominalRunS();
      message.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION;
      message.position = &pos;
    }
    break;
    case BlindsStatusCmd::SYSCMD_ACK:
    {
      ESP_LOGI(TAG, "Sending ACK for msgid %u", (unsigned)txcmd.arg);
      ack.ack_msg_id = txcmd.arg;
      ack.status     = ACK_STATUS__ACK_OK;
      // The carrier that matters — see fillPhaseReport. The hub's belief is
      // refreshed by the reply to the very command single-shot is decided
      // for, which is the only cadence that does not cost a keepalive.
      this->fillPhaseReport(ack_phase);
      ack.phase      = &ack_phase;
      message.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK;
      message.ack = &ack;
    }
    break;
    case BlindsStatusCmd::SYSCMD_GRIDSYNC_REQUEST:
    {
      // The node asks for its grid again (maybeRequestGridSync). The numbers say
      // why, so the hub's log shows what the node saw, not only that it asked.
      const int64_t now_us = nodeclock::nowUs();
      gsreq.reason          = txcmd.arg;
      gsreq.ssinceanchorfix = (this->last_anchor_fix_us_ == 0)
                                  ? 0xFFFFFFFFu
                                  : (uint32_t) ((now_us - this->last_anchor_fix_us_) / 1000000);
      gsreq.refusedsamples  = this->refused_since_fix_;
      ESP_LOGI(TAG, "Sending GRIDSYNC REQUEST (reason %u)", (unsigned) gsreq.reason);
      message.proto_case      = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_GRIDSYNCREQUEST;
      message.gridsyncrequest = &gsreq;
    }
    break;
    default:
    {

      ESP_LOGW(TAG, "Unknown TX command: %d", cmd);
    }
    }

    LoraHeader header = LORA_HEADER__INIT;
    header.destaddress = this->destAddress;
    header.destsubnet = this->destSubnet;
    header.senderaddress = sysCtrl->getConfigAddress();
    header.msgid = this->session_.nextTxId();
    message.header = &header;
    // The wake clock's stamp belongs to THIS beacon: remember which msgid the
    // next TxDone is, so noteUplinkSent can file the tick count under it.
    if (message.proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_BEACON)
    {
      this->beacon_tx_pending_msgid_ = header.msgid;
      this->beacon_tx_pending_       = true;
    }

    // F-5: persist counters periodically so an unexpected reboot resumes the
    // session without re-login.  Throttled internally to one write per margin.
    this->maybePersist_();

    uint8_t *out_buf = nullptr;
    size_t out_len = 0;
    if (pack_response_message(&message, &out_buf, &out_len))
    {
      if (!send_tx_buffer(out_buf, out_len))
      {
        ESP_LOGW(TAG, "Failed to transmit response");
      }
      free(out_buf);
    }
    else
    {
      ESP_LOGE(TAG, "Failed to pack response message for transmission");
    }
}

// The one-shot the host harness drives: a NON-BLOCKING receive plus one body.
// Returns false when the queue was empty, so a test can drain to quiescence the
// way the task does — and so "nothing was queued" is distinguishable from
// "something was and it produced no frame".
bool CmdDispatcher::runOneTxCommand()
{
  if (txCmdQueueNew == nullptr)
    return false;
  tx_command_t txcmd;
  if (xQueueReceive(txCmdQueueNew, &txcmd, 0) != pdTRUE)
    return false;
  this->serviceTxCommand(txcmd);
  return true;
}

void CmdDispatcher::processTxCommand(void *pvParameter)
{
  if (txCmdQueueNew == nullptr)
  {
    ESP_LOGE(TAG, "TX Command queue is not initialized");
    return;
  }

  const TickType_t blockTime = portMAX_DELAY;

  for (;;) // A Task shall never return or exit.
  {
    tx_command_t txcmd;
    if (xQueueReceive(txCmdQueueNew, &(txcmd), blockTime))
      this->serviceTxCommand(txcmd);
  }
}

bool CmdDispatcher::send_tx_buffer(const uint8_t *buf, size_t len)
{
  LoraInterface::rx_buffer_t *tx_buffer = loraIf->get_free_tx_buffer(10);
  if (!tx_buffer)
  {
    ESP_LOGW(TAG, "No free buffers available, dropping %u bytes", len);
    return false;
  }

  memcpy(tx_buffer->data, buf, len);
  tx_buffer->length = len;
  tx_buffer->timestamp = xTaskGetTickCount();

  if (xQueueSend(loraIf->tx_memory_pool.data_queue, &tx_buffer, 0) != pdTRUE)
  {
    ESP_LOGW(TAG, "Data queue full, dropping data");
    loraIf->return_tx_buffer_to_pool(tx_buffer);
    return false;
  }

  ESP_LOGI(TAG, "%u bytes, queued in TX buffer %p", len, tx_buffer);
  return true;
}

bool CmdDispatcher::pack_response_message(const LoraClientResponseMessage *message, uint8_t **out_buf, size_t *out_len)
{
  if (!message || !out_buf || !out_len)
  {
    return false;
  }

  uint32_t base_nonce;
  // REGISTER must ALWAYS go out as plaintext: it is the session-establishment
  // message and the hub processes it on a fast-path *before* any crypto or
  // replay/msgid check.  After a power-cycle the base nonce is restored from
  // NVS (F-5 loadPersistentState), so encrypting here would wrap REGISTER in an
  // EncryptedPayload with msgid=1; the still-running hub would then drop it as a
  // replay (msgid 1 < its stale rx counter) and never re-register the node.
  if (message->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER ||
      !message->header || !get_base_nonce(message->header->destaddress, base_nonce))
  {
    *out_len = lora_client_response_message__get_packed_size(message);
    *out_buf = static_cast<uint8_t *>(malloc(*out_len));
    if (!*out_buf)
    {
      return false;
    }
    lora_client_response_message__pack(message, *out_buf);
    return true;
  }

  uint8_t key[kAesGcmKeyBytes];
  if (!derive_aes_gcm_key(key))
  {
    ESP_LOGE(TAG, "Failed to derive AES-GCM key");
    return false;
  }

  // Use the header msgid as the unified frame counter — one counter per
  // direction serves both replay protection and nonce derivation.
  uint64_t frame_counter = static_cast<uint64_t>(message->header->msgid);
  uint8_t iv[kAesGcmIvBytes];
  if (!derive_gcm_nonce(message->header->destaddress, frame_counter, iv))
  {
    ESP_LOGE(TAG, "Failed to derive GCM nonce");
    return false;
  }

  LoraHeader outer_header = LORA_HEADER__INIT;
  outer_header.destaddress = message->header->destaddress;
  outer_header.destsubnet = message->header->destsubnet;
  outer_header.senderaddress = message->header->senderaddress;
  outer_header.msgid = message->header->msgid;

  uint8_t aad[16];
  size_t aad_len = 0;
  if (!build_header_aad(&outer_header, aad, &aad_len))
  {
    ESP_LOGE(TAG, "Failed to build AAD for encrypted response");
    return false;
  }

  // Encrypt the payload ONLY — the inner message's header is redundant (the
  // receiver uses the plaintext outer header), so strip it before packing.
  LoraClientResponseMessage inner_msg = *message;
  inner_msg.header = nullptr;
  size_t inner_len = lora_client_response_message__get_packed_size(&inner_msg);
  uint8_t *inner_buf = static_cast<uint8_t *>(malloc(inner_len));
  if (!inner_buf)
  {
    return false;
  }
  lora_client_response_message__pack(&inner_msg, inner_buf);

  uint8_t *ciphertext = static_cast<uint8_t *>(malloc(inner_len));
  if (!ciphertext)
  {
    free(inner_buf);
    return false;
  }

  uint8_t tag[kAesGcmTagBytes];
  if (!encrypt_payload_gcm(key, iv, aad, aad_len, inner_buf, inner_len, ciphertext, tag, kAesGcmTagBytes))
  {
    ESP_LOGE(TAG, "Encryption failed for response message");
    free(inner_buf);
    free(ciphertext);
    return false;
  }

  EncryptedPayload encrypted_payload = ENCRYPTED_PAYLOAD__INIT;
  // Lightweight on-air format: iv and aad are NOT transmitted.  Both are fully
  // reconstructable by the receiver from data it already has:
  //   iv  = base_nonce(peer, from login) || (uint64) header.msgId
  //   aad = build_header_aad(header)  (the plaintext outer header is sent as-is)
  // The receiver re-derives them; sending them would just be ~34 redundant
  // bytes per frame.  key_id stays empty (single fixed key).  tag + ciphertext
  // are the only mandatory payload fields.
  encrypted_payload.tag.data = tag;
  encrypted_payload.tag.len = kAesGcmTagBytes;
  encrypted_payload.ciphertext.data = ciphertext;
  encrypted_payload.ciphertext.len = inner_len;

  LoraClientResponseMessage outer_message = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
  outer_message.header = &outer_header;
  outer_message.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ENCRYPTED;
  outer_message.encrypted = &encrypted_payload;

  *out_len = lora_client_response_message__get_packed_size(&outer_message);
  *out_buf = static_cast<uint8_t *>(malloc(*out_len));
  if (!*out_buf)
  {
    free(inner_buf);
    free(ciphertext);
    return false;
  }

  lora_client_response_message__pack(&outer_message, *out_buf);
  free(inner_buf);
  free(ciphertext);
  return true;
}

bool CmdDispatcher::encrypt_payload_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                                       const uint8_t *plain, size_t plain_len, uint8_t *cipher, uint8_t *tag, size_t tag_len)
{
  (void)key; // key material is already loaded into aes_gcm_key_id_
  if (!nonce || !plain || !cipher || !tag)
    return false;

  // F-22: Use the pre-imported key slot instead of importing on every call.
  if (aes_gcm_key_id_ == PSA_KEY_ID_NULL && !init_psa_key())
  {
    ESP_LOGE(TAG, "encrypt_payload_gcm: PSA key not available");
    return false;
  }

  // Delegate to the single shared GCM primitive (comm_utils) — the same code the
  // host unit test exercises, so the test proves the production crypto path.
  return encrypt_gcm_with_key_id(aes_gcm_key_id_, nonce, aad, aad_len,
                                 plain, plain_len, cipher, tag, tag_len);
}

bool CmdDispatcher::decrypt_payload_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                                       const uint8_t *cipher, size_t cipher_len, const uint8_t *tag, size_t tag_len,
                                       uint8_t *plain_out)
{
  (void)key; // key material is already loaded into aes_gcm_key_id_
  if (!nonce || !cipher || !tag || !plain_out)
    return false;

  // F-22: Use the pre-imported key slot instead of importing on every call.
  if (aes_gcm_key_id_ == PSA_KEY_ID_NULL && !init_psa_key())
  {
    ESP_LOGE(TAG, "decrypt_payload_gcm: PSA key not available");
    return false;
  }

  // Delegate to the single shared GCM primitive (comm_utils).
  return decrypt_gcm_with_key_id(aes_gcm_key_id_, nonce, aad, aad_len,
                                 cipher, cipher_len, tag, tag_len, plain_out);
}

bool CmdDispatcher::derive_gcm_nonce(uint32_t peer_address, uint64_t frame_counter, uint8_t nonce_out[12])
{
  uint32_t base_nonce;
  if (!get_base_nonce(peer_address, base_nonce))
  {
    return false;
  }
  return framecrypto::deriveIv(base_nonce, frame_counter, nonce_out);
}

bool CmdDispatcher::get_base_nonce(uint32_t peer_address, uint32_t &base_nonce_out)
{
  return this->session_.getBaseNonce(peer_address, base_nonce_out);
}

void CmdDispatcher::set_base_nonce(uint32_t peer_address, uint32_t base_nonce)
{
  // SessionManager reports whether this peer is the one we persist for; the
  // base nonce is the critical secret for resuming, so save it immediately.
  if (this->session_.setBaseNonce(peer_address, base_nonce))
    this->session_.save();
}

void CmdDispatcher::clear_base_nonce(uint32_t peer_address)
{
  this->session_.clearBaseNonce(peer_address);
}

void CmdDispatcher::noteDeepSleepRequest(uint64_t requested_us, uint64_t applied_us)
{
  s_prev_sleep_requested_us = requested_us;
  s_prev_sleep_applied_us   = applied_us;
}

void CmdDispatcher::sendAvailable()
{
  blinds_syscmd_base_t cmd = BlindsStatusCmd::SYSCMD_AVAILABLE;
  this->setStatus(cmd);
}

void CmdDispatcher::sendPosition()
{
  // Battery voltage is now read from the LKG cache (s_lastBatteryVoltage) by
  // processTxCommand — no per-packet measurement trigger needed here.
  // taskBatteryMonitor refreshes the cache on its 5-minute interval and
  // whenever measureAndSendBatteryVoltage() is called at motor end.
  blinds_syscmd_base_t cmd = BlindsStatusCmd::SYSCMD_POSITION;
  this->setStatus(cmd);
}

void CmdDispatcher::sendBatteryVoltage()
{

  // Schedula a transmission of the battery status
  blinds_syscmd_base_t cmd = BlindsStatusCmd::SYSCMD_BATTERY;
  this->setStatus(cmd);
}

void CmdDispatcher::measureAndSendBatteryVoltage()
{
  // P2: trigger a battery MEASURE-then-SEND on the battery task.  This used to
  // ALSO call sendBatteryVoltage() right here, which transmitted the stale LKG
  // cache (0 V right after a boot/wake, before the queued measurement had run) —
  // the source of the observed 0 V reports.  The send now happens inside
  // taskBatteryMonitor, after the fresh reading is taken.
  uint8_t batteryMeasureCmd = 1;
  xQueueSend(motCtrl->motorBatteryQueue, &(batteryMeasureCmd), (TickType_t)0);
}

void CmdDispatcher::setBatteryVoltage(float voltage)
{
  // read from processTxCommand.  A plain float write is atomic on ESP32
  // (4-byte aligned, single-cycle store), so no lock is needed here.
  s_lastBatteryVoltage = voltage;
}

float CmdDispatcher::getLastBatteryVoltage() const
{
  // Same atomicity argument as setBatteryVoltage() above.
  return s_lastBatteryVoltage;
}

void CmdDispatcher::sendRegister()
{
  blinds_syscmd_base_t cmd = BlindsStatusCmd::SYSCMD_REGISTER;
  this->setStatus(cmd);
  // A REGISTER gives up the session (the TX path clears the base nonce), so
  // until a LoginMsg installs a new one this node cannot hear the hub. If the
  // REGISTER is lost nobody else will notice: keep asking. Armed without
  // armRegisterRetry()'s "already have a session" check, because the nonce is
  // only cleared later, when the TX task actually sends the frame.
  this->startRegisterRetry_();
}

// P2: classify why we are awake.  Deep-sleep wake causes take priority over the
// reset reason: a timer/EXT1 wake IS a reset from esp_reset_reason()'s point of
// view (ESP_RST_DEEPSLEEP), so checking the reset reason first would mislabel
// every scheduled wake.
WakeReason CmdDispatcher::classifyWakeReason()
{
  const uint32_t causes = esp_sleep_get_wakeup_causes();

  if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1))
  {
    // EXT1 is NOT synonymous with "a button was pressed".  The wake mask armed
    // in enterDeepsleep() contains the three buttons AND the LoRa DIO0/DIO1
    // lines, so the pin mask has to be consulted to tell them apart.
    //
    // Getting this wrong is not cosmetic: WAKE_BUTTON makes main.cpp call
    // enterInteractiveMode(), which suspends automatic mode for the whole
    // interactive_timeout (5 min here, 30 min on node 1) and keeps the node
    // awake — so a single radio interrupt would silently stop the schedule with
    // nobody having touched the blind.
    //
    // In practice the SX1278 is put in sleep mode before deep sleep and its DIO
    // lines should stay low, which is presumably why this never showed up. That
    // is an argument for it being rare, not for it being impossible.
    const uint64_t pins = esp_sleep_get_ext1_wakeup_status();
    const uint64_t button_mask = (1ULL << ctrlButtonUpPin) |
                                 (1ULL << ctrlButtonDownPin) |
                                 (1ULL << ctrlButtonStopPin);

    if (pins & button_mask)
    {
      // A button really did pull one of its pins high.  In auto mode (P3) this
      // is also what flips the node back to interactive.
      return WAKE_REASON__WAKE_BUTTON;
    }

    // EXT1 from something that is not a button — the radio lines. Treat it as
    // an ordinary check-in: beacon, let the hub say its piece, sleep again.
    // Deliberately NOT reported as a button, so automatic mode is left alone.
    ESP_LOGW(TAG, "EXT1 wake with no button pin set (mask 0x%08x%08x) — "
                  "treating as a check-in, not a button press",
             (unsigned) (pins >> 32), (unsigned) (pins & 0xFFFFFFFFu));
    return WAKE_REASON__WAKE_TIMER_CHECKIN;
  }
  if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER))
  {
    // Once a schedule exists (P3) this splits into WAKE_TIMER_EVENT vs
    // WAKE_TIMER_CHECKIN depending on whether an entry is actually due.
    // Until then every timer wake is a plain check-in.
    return WAKE_REASON__WAKE_TIMER_CHECKIN;
  }

  const esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_POWERON || reason == ESP_RST_SW ||
      reason == ESP_RST_EXT     || reason == ESP_RST_USB)
  {
    return WAKE_REASON__WAKE_BOOT;
  }
  // Panic / WDT / brownout: a crash-like reset, not a planned wake.  Worth
  // distinguishing on the hub side — a node that keeps reporting UNKNOWN is
  // reset-looping, which is exactly the failure the battery-silence outage was.
  return WAKE_REASON__WAKE_UNKNOWN;
}

// The node's timing measurement, as carried on every uplink that has room for
// it (the wake beacon and every CommandAck).
//
// This is section 4.6's promotion evidence, and it was the missing half: the
// beacon's six flat phase fields were declared on the wire, decoded by the hub
// and fed into the promotion path, and NOTHING EVER ASSIGNED THEM. Every one
// went out as a proto3 zero, the hub's guard requires samples and a crystal, so
// the guard failed closed and single-shot never engaged for any node.
//
// `samples` carries the honesty: 0 means the rest of this message says nothing,
// and the hub's guard reads it that way. On a wake beacon it is usually 0 —
// phase::Stats is a plain member, not RTC memory, so it is zeroed on every
// deep-sleep wake, and the beacon goes out before this wake has heard a single
// grid-aligned frame. The ACK is the carrier that does the work: every
// addressed frame feeds the phase tracker, so a node acking a command has just
// taken a sample against the frame it is acking.
//
// ppm is populated only once a DriftTest has produced a fit. Until then
// ppmSamples is 0, which is what the field's own comment says it means.
void CmdDispatcher::fillPhaseReport(PhaseReport &pr) const
{
  pr.samples      = this->phase_.n;
  pr.errus        = this->phase_.mean_us();
  pr.spreadus     = this->phase_.spread_us();
  pr.outsideguard = this->phase_.outside_guard;
  pr.rtcslowsrc   = (uint32_t) this->rtc_slow_src_;
  if (this->drift_fit_.ready())
  {
    pr.ppmestimate = this->drift_fit_.ppm();
    pr.ppmsamples  = this->drift_fit_.n;
  }
  // U-2: the complement of everything above. The fields so far measure the
  // hub's frames against the marks the node ARMED; a mark that was never armed
  // is invisible to all of them, WMR included, because noteMarkArmed() is what
  // opens a mark. So a node whose radio is wedged reports a perfect
  // window-mark rate while hearing nothing — the one Mode B failure the KPIs
  // cannot see. LoraInterface has counted it since it was added and nothing
  // carried it anywhere, which is the second way it stayed unseeable.
  if (this->loraIf != NULL)
    pr.rxbusyskips = this->loraIf->rxBusySkips();
  // The node's own decision, which the hub follows for single-shot.
  pr.timedrxactive  = this->timedRxActive();
  pr.demotionreason = this->demotionReasonNow();
}

// NOTE ON TIMING: the beacon must NOT be sent during the register->login
// window.  A REGISTER resets our tx counter to 0, but the hub still holds the
// rx counter from the previous session (restored from NVS), so anything we send
// before CMD_LOGIN resynchronises both sides is rejected as
// "duplicate or old message ID".  Observed live: beacons with msgid 2 and 3
// silently dropped against a hub whose rx counter was 11.
//
// So the beacon goes out at one of two points, never at boot:
//   * resume path  — counters continue unbroken from NVS, so boot is safe;
//   * register path — after CMD_LOGIN has reset both sides to 0.
void CmdDispatcher::sendWakeBeacon(WakeReason reason)
{
  this->setStatus(BlindsStatusCmd::SYSCMD_BEACON, static_cast<uint32_t>(reason));
}

// ---------------------------------------------------------------------------
// P2b: resume-first wake.
//
// A provisioned node waking with a valid persisted session can skip the whole
// REGISTER -> config -> login sequence (~4 s of awake radio) and simply
// announce itself with an encrypted beacon.  If the hub can decrypt it, the
// session is live and nothing more is needed.
//
// The risk this guards is the one failure this system cannot tolerate: the hub
// rebooted while we slept, cannot decrypt anything we send, and we sit there
// believing we are connected — a silent node.  So the resume path is always
// armed with a fallback: if no DECRYPTED downlink arrives before the timer
// expires, fall back to the full REGISTER handshake.
// ---------------------------------------------------------------------------
void CmdDispatcher::noteSessionProven()
{
  this->session_proven_ = true;
  // A decrypted downlink IS the beacon's acknowledgement, so stop the ladder.
  this->cancelBeaconRetry();

  // TIER 0: stop the resume fallback NOW, rather than letting it run to
  // expiry and discover the same fact later.
  //
  // The fallback exists to catch a hub that rebooted while we slept: if no
  // DECRYPTED downlink arrives, re-register. A decrypted downlink has just
  // arrived, so its question is answered and there is nothing left to wait
  // for. Measured before this change: TimeSync decrypted at 4.7 s, timer
  // still expiring at 13.6 s to log "session resumed OK" — nine seconds of
  // waiting for information already in hand.
  //
  // Harmless today, because the 20 s quiet window outlasts it. It becomes the
  // binding constraint the moment that window shrinks, which is exactly what
  // the sleepOk flag does.
  if (this->resume_timer_ != nullptr)
    esp_timer_stop(this->resume_timer_);
}

// ---------------------------------------------------------------------------
// Unprovisioned-node REGISTER retry.
//
// Address 0 is the "never been configured" state.  In it the node rejects every
// downlink addressed to a real short address — including the hub's LoginMsg,
// which is the only frame carrying request_register.  So an unprovisioned node
// cannot be told to re-register; it has to keep asking.
//
// Before this, the boot REGISTER was sent exactly once.  One lost frame — a
// collision with the other node's traffic is enough — left the node stranded
// until somebody physically reset it.  Seen today on node 2 after a full flash
// reset its stored config: it sat at "Config Address: 0" logging
// "This message is not for me" at every login attempt.
// ---------------------------------------------------------------------------
bool CmdDispatcher::isProvisioned()
{
  return sysCtrl->getConfigAddress() != 0;
}

// A usable session: a base nonce for the hub this node persists for. Sending
// REGISTER throws it away (SYSCMD_REGISTER clears it), and only a LoginMsg
// installs a new one.
//
// Why the retry also covers a PROVISIONED node (2026-09-13, node 2, fw 1.0.66):
// the boot REGISTER hit a busy channel and never reached the hub. The node had
// already dropped its restored nonce; the hub, never having seen the REGISTER,
// kept the old session and sent ScheduleConfig and GridSync encrypted under it.
// The node dropped every one ("No base nonce for peer", DEBUG level), its
// uplinks went out in the clear and were accepted, and nothing on either side
// noticed. It stayed deaf until the hub was restarted. A provisioned node
// without a session is exactly as stranded as an unprovisioned one.
bool CmdDispatcher::hasHubSession_()
{
  uint32_t nonce = 0;
  return this->session_.getBaseNonce(this->session_.persistPeer(), nonce);
}

void CmdDispatcher::registerRetryCb(void *arg)
{
  CmdDispatcher *self = static_cast<CmdDispatcher *>(arg);
  if (self == nullptr)
    return;

  if (self->isProvisioned() && self->hasHubSession_())
  {
    self->cancelRegisterRetry();
    return;
  }

  ESP_LOGW(TAG, "%s — re-sending REGISTER",
           self->isProvisioned() ? "No session with the hub" : "Still unprovisioned");
  self->sendRegister();
}

void CmdDispatcher::armRegisterRetry()
{
  if (this->isProvisioned() && this->hasHubSession_())
    return;
  this->startRegisterRetry_();
}

void CmdDispatcher::startRegisterRetry_()
{
  const esp_timer_create_args_t args = {
      .callback              = &CmdDispatcher::registerRetryCb,
      .arg                   = this,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "register_retry",
      .skip_unhandled_events = true,
  };
  if (this->register_retry_timer_ == nullptr &&
      esp_timer_create(&args, &this->register_retry_timer_) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to create REGISTER retry timer");
    return;
  }

  esp_timer_stop(this->register_retry_timer_);
  // Periodic, not one-shot: the hub may be rebooting, out of range, or busy
  // with the other node.  There is nothing else a node without an address or
  // a session can usefully do, so it keeps asking rather than going quiet.
  esp_timer_start_periodic(this->register_retry_timer_,
                           (uint64_t) kRegisterRetryMs * 1000ULL);
  ESP_LOGI(TAG, "REGISTER retry armed every %u s until the hub logs us in",
           (unsigned) (kRegisterRetryMs / 1000u));
}

void CmdDispatcher::cancelRegisterRetry()
{
  if (this->register_retry_timer_ == nullptr)
    return;
  esp_timer_stop(this->register_retry_timer_);
  ESP_LOGI(TAG, "Provisioned — REGISTER retry cancelled");
}

// ---------------------------------------------------------------------------
// P3: automatic mode.
// ---------------------------------------------------------------------------

// RTC-backed record of the last entry we actually executed.  Survives deep
// sleep (plain RAM does not), so a node that wakes twice inside one event's
// minute — or reboots straight after executing — cannot run it twice and
// double-move the blind.

static RTC_DATA_ATTR uint64_t s_last_exec_epoch = 0;

// D4: temporary interactive override.  Epoch at which automatic mode resumes.
// 0 = no override; kInteractiveForever = override that never expires (the
// documented interactiveTimeout == 0 case).  RTC-backed so a wake inside the
// window does not silently resume auto mode early.
static constexpr uint64_t kInteractiveForever = UINT64_MAX;
static RTC_DATA_ATTR uint64_t s_interactive_until = 0;

void CmdDispatcher::interactiveExpiredCb(void *arg)
{
  CmdDispatcher *self = static_cast<CmdDispatcher *>(arg);
  if (self == nullptr)
    return;

  // Re-check rather than assume: a button pressed while the timer was pending
  // will have pushed the deadline out, and the hub may have changed the mode.
  if (self->isTemporarilyInteractive())
    return;
  if (!self->shouldRunAutoMode())
    return;

  ESP_LOGI(TAG, "Interactive window expired — returning to automatic mode");
  self->enterDeepsleep();
}

void CmdDispatcher::enterInteractiveMode()
{
  const uint32_t timeout = sysCtrl->getInteractiveTimeout();

  // Drop any sleep already counting down.  A person pressing the button must
  // not watch the blind go unresponsive because a quiet-window timer armed
  // seconds earlier is still due to fire.
  this->cancelAutoSleep();

  if (timeout == 0)
  {
    s_interactive_until = kInteractiveForever;
    ESP_LOGI(TAG, "Interactive mode (no timeout configured — staying interactive)");
    return;
  }

  struct timeval now;
  gettimeofday(&now, NULL);
  s_interactive_until = static_cast<uint64_t>(now.tv_sec) + timeout;

  ESP_LOGI(TAG, "Interactive mode for %u s (auto mode suspended, not disabled)",
           (unsigned) timeout);

  // Arm the return.  Without this the node would stay awake until something
  // else happened to put it to sleep — which on a battery node is the whole
  // cost of the feature.
  const esp_timer_create_args_t args = {
      .callback              = &CmdDispatcher::interactiveExpiredCb,
      .arg                   = this,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "interactive_end",
      .skip_unhandled_events = true,
  };
  if (this->interactive_timer_ == nullptr &&
      esp_timer_create(&args, &this->interactive_timer_) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to create interactive-return timer — auto mode will "
                  "resume at the next wake instead");
    return;
  }
  esp_timer_stop(this->interactive_timer_); // restart the window on a new press
  esp_timer_start_once(this->interactive_timer_,
                       static_cast<uint64_t>(timeout) * 1000000ULL);
}

void CmdDispatcher::clearInteractiveMode()
{
  s_interactive_until = 0;
  if (this->interactive_timer_ != nullptr)
    esp_timer_stop(this->interactive_timer_);
}

bool CmdDispatcher::isTemporarilyInteractive()
{
  if (s_interactive_until == 0)
    return false;
  if (s_interactive_until == kInteractiveForever)
    return true;
  if (!s_clock_valid)
  {
    // No clock means the deadline cannot be evaluated. Treat the override as
    // still active: staying responsive is the safe failure here, whereas
    // sleeping on an unevaluable deadline could strand the node.
    return true;
  }
  struct timeval now;
  gettimeofday(&now, NULL);
  if (static_cast<uint64_t>(now.tv_sec) >= s_interactive_until)
  {
    s_interactive_until = 0; // expired — clear so the check stays cheap
    return false;
  }
  return true;
}

uint32_t CmdDispatcher::interactiveRemaining()
{
  if (s_interactive_until == 0)
    return 0;
  if (s_interactive_until == kInteractiveForever)
    return UINT32_MAX;
  if (!s_clock_valid)
    return UINT32_MAX;
  struct timeval now;
  gettimeofday(&now, NULL);
  const uint64_t n = static_cast<uint64_t>(now.tv_sec);
  return (n >= s_interactive_until) ? 0 : static_cast<uint32_t>(s_interactive_until - n);
}

uint64_t CmdDispatcher::computeNextEvent()
{
  if (!s_clock_valid)
    return 0;
  struct timeval now;
  gettimeofday(&now, NULL);
  return sched::next_occurrence(sysCtrl->getEntries(), sysCtrl->getEntryCount(),
                                static_cast<uint64_t>(now.tv_sec), s_utc_offset_s);
}

bool CmdDispatcher::shouldRunAutoMode()
{
  // The decision itself is pure and lives in AutoModePolicy, where it is
  // testable without a dispatcher. Only the reaction stays here.
  const automode::Refusal why = automode::refusalFor(
      sysCtrl->getAutoMode(),
      this->isTemporarilyInteractive(),
      s_clock_valid,
      sysCtrl->hasUsableSchedule());

  switch (why)
  {
  case automode::Refusal::None:
    return true;

  case automode::Refusal::NoClock:
    // I8: never sleep against a schedule we cannot evaluate. This branch IS
    // the discovery that we need a TimeSync, so arm the retry here — it covers
    // every path that can reach it without each having to remember to.
    ESP_LOGW(TAG, "Auto mode is on but the clock is not valid yet — staying interactive");
    this->armClockRetry();
    return false;

  case automode::Refusal::NoUsableSchedule:
    ESP_LOGW(TAG, "Auto mode requested but no usable schedule — staying interactive");
    return false;

  case automode::Refusal::ModeOff:
  case automode::Refusal::TemporarilyInteractive:
  default:
    return false;
  }
}

// Seconds to sleep before the next wake, or 0 when we must not sleep.
uint64_t CmdDispatcher::computeSleepSeconds()
{
  if (!this->shouldRunAutoMode())
    return 0;

  struct timeval now;
  gettimeofday(&now, NULL);

  return automode::sleepSeconds(static_cast<uint64_t>(now.tv_sec),
                                this->computeNextEvent(),
                                sysCtrl->getBeaconLead(),
                                sysCtrl->getCheckinInterval());
}

// Execute whatever is due right now, including anything missed while we were
// off.  Returns true if the motor was actually commanded.
bool CmdDispatcher::runDueScheduleEntry()
{
  if (!this->shouldRunAutoMode())
    return false;

  struct timeval now;
  gettimeofday(&now, NULL);
  const uint64_t now_s = static_cast<uint64_t>(now.tv_sec);

  // catchup_window governs REPLAYING A MISS, not whether the schedule runs.
  //
  // This used to `return false` outright when catchup was 0, which silently
  // disabled automatic mode altogether: runDueScheduleEntry() is the ONLY thing
  // that executes an entry, so with catchup 0 the node woke on time, beaconed,
  // and never moved the blind. Observed live on node 1 — two beacons either
  // side of its 21:45 close, and the blind untouched.
  //
  // The documented meaning of 0 is "never execute a MISSED event". An entry
  // falling due right now is not a missed event, so a small fixed grace window
  // is always searched. It has to be a window rather than an instant because
  // the node reaches this call a few seconds after the event at best, and much
  // later if the hub kept it awake (see the quiet-window re-check).
  const uint32_t lookback = automode::lookbackSeconds(sysCtrl->getCatchupWindow());

  // Look back over that window for the LATEST entry that should have fired.
  // Latest, not first: replaying a missed morning OPEN after a missed midday
  // CLOSE would leave the blind in the wrong end state.
  const uint64_t from = (now_s > lookback) ? (now_s - lookback) : 0;
  int which = -1;
  const uint64_t due = sched::last_missed(sysCtrl->getEntries(), sysCtrl->getEntryCount(),
                                          from, now_s, s_utc_offset_s, &which);
  if (due == 0 || which < 0)
    return false;

  if (automode::alreadyExecuted(due, s_last_exec_epoch))
  {
    ESP_LOGI(TAG, "Schedule entry %d already executed — skipping", which);
    return false;
  }

  const sched::Entry &e = sysCtrl->getEntries()[which];
  char when[32];
  this->formatLocalTime(due, when, sizeof(when));
  ESP_LOGI(TAG, "Executing schedule entry %d (due %s, action=%u)",
           which, when, (unsigned) e.action);

  s_last_exec_epoch = due;

  switch (e.action)
  {
  case sched::ACTION_OPEN:
    this->setBlindOperation(BlindsOpCmd::SYSCMD_OPEN);
    break;
  case sched::ACTION_CLOSE:
    this->setBlindOperation(BlindsOpCmd::SYSCMD_CLOSE);
    break;
  case sched::ACTION_STOP:
    this->setBlindOperation(BlindsOpCmd::SYSCMD_STOP);
    break;
  case sched::ACTION_POSITION:
  {
    const float target = static_cast<float>(e.positionPct) / 100.0f;
    motCtrl->setTargetPosition(target);
    this->setBlindOperation(motCtrl->getPosition() > target ? BlindsOpCmd::SYSCMD_CLOSE
                                                            : BlindsOpCmd::SYSCMD_OPEN);
    break;
  }
  default:
    ESP_LOGW(TAG, "Unknown schedule action %u — ignored", (unsigned) e.action);
    return false;
  }
  return true;
}

bool CmdDispatcher::canResumeSession()
{
  uint32_t unused = 0;
  return this->session_.hasValidState() &&
         this->session_.getBaseNonce(this->destAddress, unused);
}

void CmdDispatcher::resumeFallbackCb(void *arg)
{
  CmdDispatcher *self = static_cast<CmdDispatcher *>(arg);
  if (self == nullptr)
    return;

  if (self->session_proven_)
  {
    ESP_LOGI(TAG, "P2b: session resumed OK — skipped REGISTER handshake");
    return;
  }

  ESP_LOGW(TAG, "P2b: no decrypted downlink within %u ms — falling back to REGISTER",
           (unsigned) kResumeFallbackMs);
  self->sendRegister();
}

void CmdDispatcher::autoSleepCb(void *arg)
{
  auto *self = static_cast<CmdDispatcher *>(arg);

  // Re-check rather than trusting the condition that armed us: a button press
  // or a mode change may have happened during the quiet window, and those must
  // win over a sleep that was decided seconds ago.
  if (!self->shouldRunAutoMode())
  {
    ESP_LOGI(TAG, "Auto-sleep window elapsed but auto mode no longer applies — staying awake");
    return;
  }

  // Run anything that fell due WHILE we were awake, before working out when to
  // wake next.
  //
  // The node wakes beacon_lead (30 s) BEFORE its event, so at boot nothing is
  // due yet and runDueScheduleEntry() correctly does nothing.  Normally the
  // sleep that follows is computed as "next event minus lead", which clamps to
  // now+1 s for an imminent event, so the node naps and wakes to execute it.
  //
  // But if the hub keeps talking past the event time — every downlink refreshes
  // this window — that nap never happens, and by the time we get here
  // next_occurrence() has already moved PAST the event.  The entry was then
  // skipped outright.  Observed live: woken 15:27:23 for a 15:28:00 CLOSE,
  // still awake at 15:28:03, and the node slept until 15:36 having never moved
  // the blind.
  //
  // Queued before the sleep, so taskDeepSleep drains the motor command first —
  // the same ordering the boot path uses.
  if (self->runDueScheduleEntry())
    ESP_LOGI(TAG, "Auto mode: executed an entry that fell due while awake");

  char next_str[32] = "none";
  const uint64_t next = self->computeNextEvent();
  if (next != 0)
    CmdDispatcher::formatLocalTime(next, next_str, sizeof(next_str));
  ESP_LOGI(TAG, "Auto mode: hub quiet for %u ms — sleeping, next event %s",
           (unsigned) self->autoSleepQuietMs(), next_str);

  // Queued, never inline: this is a timer task, and tearing the radio down
  // outside processSysCommand's task crashed the node when it was tried.
  self->setSystemCommand(BlindsSysCmd::SYSCMD_SLEEP);
}

void CmdDispatcher::cancelAutoSleep()
{
  if (this->auto_sleep_timer_ != nullptr)
    esp_timer_stop(this->auto_sleep_timer_);
}

void CmdDispatcher::armAutoSleep()
{
  const esp_timer_create_args_t args = {
      .callback              = &CmdDispatcher::autoSleepCb,
      .arg                   = this,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "auto_sleep",
      .skip_unhandled_events = true,
  };

  if (this->auto_sleep_timer_ == nullptr &&
      esp_timer_create(&args, &this->auto_sleep_timer_) != ESP_OK)
  {
    // Without the timer the choice is "sleep now" or "never sleep".  Sleep now:
    // a node that never sleeps flattens its battery, whereas sleeping early
    // only costs this cycle's handshake, which the next wake retries.
    ESP_LOGE(TAG, "Failed to create auto-sleep timer — sleeping immediately instead");
    this->setSystemCommand(BlindsSysCmd::SYSCMD_SLEEP);
    return;
  }

  // Restart, don't stack: each refresh pushes the sleep out by a full window.
  esp_timer_stop(this->auto_sleep_timer_);   // no-op if not running
  ESP_ERROR_CHECK(esp_timer_start_once(this->auto_sleep_timer_,
                                       (uint64_t) this->autoSleepQuietMs() * 1000ULL));
}

uint32_t CmdDispatcher::parseFirmwareVersion(const char *v)
{
  if (v == nullptr)
    return 0;

  // Accept a leading 'v' ("v1.0.17"), which some build setups prepend.
  if (*v == 'v' || *v == 'V')
    ++v;

  uint32_t part[3] = {0, 0, 0};
  int idx = 0;
  bool any_digit = false;

  for (; *v != 0 && idx < 3; ++v)
  {
    if (*v >= '0' && *v <= '9')
    {
      part[idx] = part[idx] * 10u + static_cast<uint32_t>(*v - '0');
      any_digit = true;
    }
    else if (*v == '.')
    {
      ++idx;
    }
    else
    {
      // Stop at a suffix such as "1.0.17-dirty" rather than folding its digits
      // into the patch number.
      break;
    }
  }

  if (!any_digit)
    return 0;
  // Keep each field in its lane: an out-of-range component would otherwise
  // carry into the next and produce a version that compares wrongly.
  if (part[0] > 99 || part[1] > 99 || part[2] > 99)
    return 0;

  return part[0] * 10000u + part[1] * 100u + part[2];
}

uint32_t CmdDispatcher::firmwareVersion()
{
  // Cached: the running image cannot change under us.
  static uint32_t cached = 0;
  if (cached != 0)
    return cached;

  const esp_app_desc_t *desc = esp_app_get_description();
  cached = CmdDispatcher::parseFirmwareVersion(desc != nullptr ? desc->version : nullptr);
  if (cached == 0)
    ESP_LOGW(TAG, "Could not parse firmware version '%s' - reporting 0",
             (desc != nullptr) ? desc->version : "(null)");
  return cached;
}

uint32_t CmdDispatcher::autoSleepQuietMs()
{
  const uint32_t ms = automode::quietWindowMs(sysCtrl->getPostEventWindow());
  if (ms != sysCtrl->getPostEventWindow() * 1000u)
    ESP_LOGW(TAG, "post_event_window %u ms is below the %u ms floor — clamping "
                  "(shorter than the hub's inter-frame gaps)",
             (unsigned) (sysCtrl->getPostEventWindow() * 1000u),
             (unsigned) automode::kQuietWindowMinMs);
  return ms;
}

void CmdDispatcher::clockRetryCb(void *arg)
{
  auto *self = static_cast<CmdDispatcher *>(arg);
  if (self == nullptr)
    return;

  if (CmdDispatcher::isClockValid())
  {
    // Someone else got us a clock in the meantime.
    self->cancelClockRetry();
    return;
  }

  ESP_LOGI(TAG, "Still no clock — re-sending the wake beacon to ask for a TimeSync");
  self->sendWakeBeacon(self->getWakeReason());
}

void CmdDispatcher::cancelClockRetry()
{
  if (this->clock_retry_timer_ != nullptr)
    esp_timer_stop(this->clock_retry_timer_);
}

void CmdDispatcher::armClockRetry()
{
  const esp_timer_create_args_t args = {
      .callback              = &CmdDispatcher::clockRetryCb,
      .arg                   = this,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "clock_retry",
      .skip_unhandled_events = true,
  };
  if (this->clock_retry_timer_ == nullptr &&
      esp_timer_create(&args, &this->clock_retry_timer_) != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to create clock retry timer");
    return;
  }

  // Periodic and unbounded, like the REGISTER retry: a node that wants auto
  // mode but has no clock stays awake anyway, so the beacon is not what costs
  // the battery — being stranded is. It keeps asking until answered.
  esp_timer_stop(this->clock_retry_timer_);
  esp_timer_start_periodic(this->clock_retry_timer_,
                           (uint64_t) kClockRetryMs * 1000ULL);
}

void CmdDispatcher::beaconRetryCb(void *arg)
{
  auto *self = static_cast<CmdDispatcher *>(arg);
  if (self == nullptr)
    return;

  if (self->session_proven_)
  {
    // A decrypted downlink arrived — the beacon was heard.
    self->cancelBeaconRetry();
    return;
  }

  if (self->beacon_retries_ >= kMaxBeaconRetries)
  {
    // Out of cheap options. Say so explicitly rather than going quiet: the
    // 12 s resume fallback takes it from here and re-registers.
    ESP_LOGW(TAG, "Beacon unanswered after %u retries — leaving it to the "
                  "REGISTER fallback",
             (unsigned) self->beacon_retries_);
    return;
  }

  self->beacon_retries_++;
  ESP_LOGW(TAG, "No TimeSync %u ms after the beacon — re-sending it (%u/%u)",
           (unsigned) kBeaconAckMs, (unsigned) self->beacon_retries_,
           (unsigned) kMaxBeaconRetries);
  self->sendWakeBeacon(self->getWakeReason());
  self->armBeaconRetry();
}

void CmdDispatcher::cancelBeaconRetry()
{
  if (this->beacon_retry_timer_ != nullptr)
    esp_timer_stop(this->beacon_retry_timer_);
}

void CmdDispatcher::armBeaconRetry()
{
  const esp_timer_create_args_t args = {
      .callback              = &CmdDispatcher::beaconRetryCb,
      .arg                   = this,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "beacon_retry",
      .skip_unhandled_events = true,
  };
  if (this->beacon_retry_timer_ == nullptr &&
      esp_timer_create(&args, &this->beacon_retry_timer_) != ESP_OK)
  {
    // Without the timer we simply lose the cheap retries; the 12 s REGISTER
    // fallback still covers the loss, so this is degraded, not broken.
    ESP_LOGE(TAG, "Failed to create beacon retry timer — relying on the "
                  "REGISTER fallback alone");
    return;
  }

  // Jittered so two nodes that just collided do not retry in lockstep.
  const uint32_t delay_ms = kBeaconAckMs + (esp_random() % kBeaconRetryJitterMs);
  esp_timer_stop(this->beacon_retry_timer_);
  ESP_ERROR_CHECK(esp_timer_start_once(this->beacon_retry_timer_,
                                       (uint64_t) delay_ms * 1000ULL));
}

void CmdDispatcher::armResumeFallback()
{
  this->session_proven_ = false;

  const esp_timer_create_args_t args = {
      .callback              = &CmdDispatcher::resumeFallbackCb,
      .arg                   = this,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "resume_fallback",
      .skip_unhandled_events = true,
  };

  if (this->resume_timer_ == nullptr &&
      esp_timer_create(&args, &this->resume_timer_) != ESP_OK)
  {
    // Could not arm the safety net, so do NOT take the risky path: register
    // immediately.  Losing the battery saving is vastly preferable to a node
    // that might never come back.
    ESP_LOGE(TAG, "P2b: failed to create resume-fallback timer — registering instead");
    this->sendRegister();
    return;
  }

  esp_timer_stop(this->resume_timer_); // no-op if not running
  ESP_ERROR_CHECK(esp_timer_start_once(this->resume_timer_,
                                       (uint64_t) kResumeFallbackMs * 1000ULL));
  ESP_LOGI(TAG, "P2b: resuming session — REGISTER fallback armed for %u ms",
           (unsigned) kResumeFallbackMs);

  // Cheap retries first. armResumeFallback() means "beacon sent, expecting a
  // decrypted downlink", which is exactly the state the ladder covers: two
  // re-beacons at ~4 s before escalating to the REGISTER above.
  this->beacon_retries_ = 0;
  this->armBeaconRetry();
}

void CmdDispatcher::setStatus(const uint8_t statusType, uint32_t arg)
{
  tx_command_t cmd = {statusType, arg};
  xQueueSend(txCmdQueueNew, (void *)&cmd, (TickType_t)0);
}

void CmdDispatcher::setBlindOperation(const blinds_syscmd_base_t cmd)
{
  blinds_syscmd_base_t sysCmd = cmd;
  ESP_LOGI(TAG, "   CMD OP");
  xQueueSend(rxCmdQueueNew, (void *)&sysCmd, (TickType_t)0);
}

void CmdDispatcher::setSystemCommand(const blinds_syscmd_base_t cmd)
{
  blinds_syscmd_base_t sysCmd = cmd;
  ESP_LOGI(TAG, "   CMD OP");
  xQueueSend(sysCmdQueueNew, (void *)&sysCmd, (TickType_t)0);
}

// void CmdDispatcher::setAvailable()

void CmdDispatcher::setMotorCommand(const MotorCmd_t cmd)
{

  portENTER_CRITICAL(&buttonMux);
  if (motCtrl->motorCmdQueueNew == NULL)
  {
    ESP_LOGE(TAG, "Motor command queue not initialized!");
    portEXIT_CRITICAL(&buttonMux);
    return;
  }

  xQueueSend(motCtrl->motorCmdQueueNew, (void *)&cmd, (TickType_t)0);
  portEXIT_CRITICAL(&buttonMux);
  if (xHandleMotor != NULL)
  {
    xTaskNotifyGive(xHandleMotor);
  }
}

void CmdDispatcher::processRxCommand(void *pvParameters)
{

  for (;;) // A Task shall never return or exit.
  {
    // Feed the task watchdog

    blinds_syscmd_base_t rxCmd;
    const TickType_t blockTime = portMAX_DELAY;

    if (rxCmdQueueNew != 0)
    {
      // Receive a message on the created queue.  Block for 10 ticks if a
      // message is not immediately available.
      if (xQueueReceive(rxCmdQueueNew, &(rxCmd), blockTime))
      {
        ESP_LOGI(TAG, "RX Command queue processing");

        std::string msg;
        MotorCmd_t cmd = MOTCMD_IDLE;
        switch (rxCmd)
        {
        case BlindsOpCmd::SYSCMD_OPEN:
          msg = ("Blinds up!");
          cmd = MOTCMD_FULL_UP;
          this->setMotorCommand(cmd);
          break;
        case BlindsOpCmd::SYSCMD_CLOSE:
          msg = ("Blinds down!");
          cmd = MOTCMD_FULL_DOWN;
          this->setMotorCommand(cmd);
          break;
        case BlindsOpCmd::SYSCMD_STOP:
          msg = ("Blinds stop!");
          cmd = MOTCMD_STOP;
          this->setMotorCommand(cmd);
          ;
          break;
        default:
          msg = ("Not a valid command!");
        }

        ESP_LOGI(TAG, "RX command: %s", msg.c_str());
      }
    }
  }
}

void CmdDispatcher::startBatteryMonitoring()
{
  spawnTaskBatteryMonitor();
}

// F-39: startBatteryMonitoringWithUDP / startMotorCurrentMonitorWithUDP removed
//       2026-05-25 — dead code; the UDP monitoring tasks were never used in production.

void CmdDispatcher::setBatteryMeasurementBusy(bool busy)
{
  // Plain bool write, read by checkQueuesIdle() from the deep-sleep task. Not
  // atomic and does not need to be: the only consumer treats any non-false as
  // "wait", and a torn read of a bool is not a thing on this core.
  batteryMeasurementBusy_ = busy;
}

void CmdDispatcher::startMotorCurrentMonitor()
{

  spawnTaskMotorCurrentMonitor();
}

void CmdDispatcher::startOTA()
{
  trigger_ota = 1;
  esp_restart();
}

void CmdDispatcher::startWifi()
{
  sysCtrl->setupWiFi();
}

void CmdDispatcher::stopWifi()
{
  sysCtrl->shutdownWiFi();
}

void CmdDispatcher::processSysCommand(void *pvParameters)
{

  if (sysCmdQueueNew == nullptr)
  {
    ESP_LOGE(TAG, "System command queue not initialized!");
    return;
  }

  for (;;) // A Task shall never return or exit.
  {
    blinds_syscmd_base_t rxCmd;
    const TickType_t blockTime = portMAX_DELAY;

    // Receive a message on the created queue.  Block for 10 ticks if a
    // message is not immediately available.
    if (xQueueReceive(sysCmdQueueNew, &(rxCmd), blockTime))
    {
      ESP_LOGI(TAG, "RX Command queue processing");

      std::string msg;
      switch (rxCmd)
      {
      case BlindsSysCmd::SYSCMD_ENABLE_WIFI:
        this->startWifi();
        msg = ("Wifi on!");
        break;
      case BlindsSysCmd::SYSCMD_DISABLE_WIFI:
        this->stopWifi();
        msg = ("Wifi off!");
        break;
      case BlindsSysCmd::SYSCMD_OTA:
        msg = ("Ota enabled!");
        this->startOTA();
        break;
      case BlindsSysCmd::SYSCMD_STATUS:
        // F-39: startBatteryMonitoringWithUDP removed; use standard battery monitoring.
        this->startBatteryMonitoring();
        msg = ("Status!");
        break;
      case BlindsSysCmd::SYSCMD_SLEEP:
        this->enterDeepsleep();
        msg = ("Status!");
        break;
      default:
        msg = ("Not a valid command!");
      }

      ESP_LOGI(TAG, "Sys command: %s", msg.c_str());
    }
  }
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleNotSet(LoraClientOperationMessage *message_to_process,
                                 const LoraHeader *outer_header)
{
    ESP_LOGI(TAG, "   CMD NOT_SET");
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleOperation(LoraClientOperationMessage *message_to_process,
                                    const LoraHeader *outer_header)
{
    LoraCoverOperation *op = message_to_process->operation;
    switch (op->covop_case)
    {
    case LORA_COVER_OPERATION__COVOP_OPERATION:
    {
      blinds_syscmd_base_t sysCmd = blindsOpPbToCmd(op->operation);
      ESP_LOGI(TAG, "   CMD OP");
      this->setBlindOperation(sysCmd);
    }
    break;
    case LORA_COVER_OPERATION__COVOP_POSITION:
    {
      float position = op->position;
      ESP_LOGI(TAG, "   CMD POS %.2f", position);

      motCtrl->setTargetPosition(position);
      ESP_LOGI(TAG, "Current position: %.2f", motCtrl->getPosition());
      ESP_LOGI(TAG, "Target position: %.2f", position);
      if (motCtrl->getPosition() > position)
      {
        this->setBlindOperation(BlindsOpCmd::SYSCMD_CLOSE);
        ESP_LOGI(TAG, "Command CLOSE");
      }
      else
      {
        this->setBlindOperation(BlindsOpCmd::SYSCMD_OPEN);
        ESP_LOGI(TAG, "Command OPEN");
      }
    }
    break;
    case LORA_COVER_OPERATION__COVOP__NOT_SET:
    {
      ESP_LOGI(TAG, "   CMD OP NOT_SET");
    }
    break;
    case _LORA_COVER_OPERATION__COVOP__CASE_IS_INT_SIZE:
    {
      ESP_LOGI(TAG, "   CMD OP unknown case");
    }
    break;
    }

    // F-4: Acknowledge the cover command so the hub can stop retransmitting.
    // The hub addresses commands to a specific node and retransmits with a
    // fresh (incrementing) msgid, so re-executing a retransmit is idempotent
    // (same motor target) and each copy is acknowledged independently.
    this->sendCommandAck(outer_header->msgid);
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleSysop(LoraClientOperationMessage *message_to_process,
                                const LoraHeader *outer_header)
{
    ClientOperation op = message_to_process->sysop;
    blinds_syscmd_base_t sysCmd = blindsSysPbToCmd(op);
    ESP_LOGI(TAG, "   SYS OP");

    // Mode transitions are handled HERE, not through blindsSysPbToCmd().
    //
    // They act on the dispatcher (interactive window, auto-sleep timer), not
    // on the system-command queue, so no BlindsSysCmd value means "switch
    // mode" -- and routing them through that enum is what dropped them:
    // CMD_MODE_AUTO (5) and CMD_MODE_INTERACTIVE (6) fell to `default:
    // return 0` and the node silently did nothing.
    //
    // It stayed invisible because the ack is sent regardless (the hub saw
    // "Tracked op acknowledged" and believed the mode had changed), and
    // because ScheduleConfig also carries mode -- so any change followed by a
    // schedule push landed anyway. It failed only for a node that was AWAKE
    // and not beaconing, which is precisely the case this sysop exists for.
    // Observed 2026-08-31: hub switch ON and acked, node still interactive 27
    // minutes later.
    const bool is_mode_sysop = (op == CLIENT_OPERATION__CMD_MODE_AUTO ||
                                op == CLIENT_OPERATION__CMD_MODE_INTERACTIVE);
    if (is_mode_sysop)
    {
      const bool want_auto = (op == CLIENT_OPERATION__CMD_MODE_AUTO);
      ESP_LOGI(TAG, "   Mode sysop: %s", want_auto ? "AUTO" : "INTERACTIVE");

      sysCtrl->setAutoMode(want_auto);
      sysCtrl->mountLittleFS();
      sysCtrl->saveConfiguration();   // must survive the next reboot
      sysCtrl->unmountLittleFS();

      if (want_auto)
      {
        // Drop any interactive override and arm the sleep, so an awake node
        // actually goes down instead of waiting for a beacon that is not
        // coming.
        this->clearInteractiveMode();
        if (this->shouldRunAutoMode())
          this->armAutoSleep();
        else
          ESP_LOGW(TAG, "   Auto requested but not runnable yet (needs a valid "
                        "clock and a schedule) — staying awake");
      }
      else
      {
        this->cancelAutoSleep();
        this->enterInteractiveMode();
      }
    }
    // The nightly CMD_SLEEP belongs to INTERACTIVE mode only.
    //
    // It exists to put an awake node down for sleep_duration. A node in
    // automatic mode manages its own sleep from the schedule, so obeying this
    // would replace a schedule-derived wake with a fixed-duration one and put
    // the node's next wake where the hub guesses rather than where the schedule
    // says. Ignore it and keep the schedule.
    //
    // Note this ignores only the SYSOP arriving from the hub. Auto mode's own
    // sleep uses the same SYSCMD_SLEEP internally (armAutoSleep), and that must
    // still work — which is why the check lives here and not at the dispatch
    // site in processSysCommand().
    //
    // The ack below is still sent: the hub must stop retransmitting either way,
    // and "received and deliberately ignored" is a successful delivery.
    // Guarded: a mode sysop has no BlindsSysCmd equivalent, so sysCmd is 0
    // here. Falling through would hand that 0 to setSystemCommand().
    if (is_mode_sysop)
    {
      // already handled above
    }
    else if (sysCmd == BlindsSysCmd::SYSCMD_SLEEP && sysCtrl->getAutoMode())
    {
      ESP_LOGI(TAG, "CMD_SLEEP ignored — automatic mode manages its own sleep "
                    "(next wake comes from the schedule, not sleep_duration)");
    }
    else
    {
      this->setSystemCommand(sysCmd);
    }

    // (a) Acknowledge the sysop so the hub can stop retransmitting — sysops now
    // use the SAME tracked/acked delivery path as cover ops (the hub retransmits
    // with a fresh incrementing msgid until this ack, so a dropped OTA / sleep /
    // wifi / status frame recovers instead of being lost).  Re-executing a
    // retransmitted copy is idempotent per command, exactly like a cover op.
    this->sendCommandAck(outer_header->msgid);
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleClientConfig(LoraClientOperationMessage *message_to_process,
                                       const LoraHeader *outer_header)
{
    ESP_LOGI(TAG, "   CMD CONFIG");

    ClientConfig *clientconfig = message_to_process->clientconfig;

    uint8_t mac_arr[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_arr, ESP_MAC_EFUSE_FACTORY));
    uint64_t my_mac_addr = 0;
    my_mac_addr |= mac_arr[0];
    my_mac_addr <<= 8;
    my_mac_addr |= mac_arr[1];
    my_mac_addr <<= 8;
    my_mac_addr |= mac_arr[2];
    my_mac_addr <<= 8;
    my_mac_addr |= mac_arr[3];
    my_mac_addr <<= 8;
    my_mac_addr |= mac_arr[4];
    my_mac_addr <<= 8;
    my_mac_addr |= mac_arr[5];

    ESP_LOGI(TAG, "Settings for MAC: %llu", clientconfig->mac_addr);
    ESP_LOGI(TAG, "           MyMAC: %llu", my_mac_addr);
    ESP_LOGI(TAG, "Setting new address: %d", (unsigned int)clientconfig->addr);
    ESP_LOGI(TAG, "Setting new subnet: %d", (unsigned int)clientconfig->subnt);
    ESP_LOGI(TAG, "Setting hostname: %s", (char *)clientconfig->name.data);
    if (my_mac_addr == clientconfig->mac_addr)
    {
      ESP_LOGI(TAG, "MAC address matches device, applying configuration");
    }
    else
    {
      ESP_LOGW(TAG, "MAC address does not match device, ignoring configuration");
      return;
    }
    // Did this config actually give us a different address?  An unprovisioned
    // node runs on address 0, and every frame it sent before this point —
    // including its wake beacon — carried that 0 and was dropped by the hub's
    // per-node address filter.  The REGISTER survives because it is matched by
    // MAC, which is how we got here at all.
    const bool address_changed =
        (this->sysCtrl->getConfigAddress() != (uint8_t) clientconfig->addr);

    this->sysCtrl->setAddress(clientconfig->addr, clientconfig->subnt);
    this->sysCtrl->setSleepDuration(clientconfig->sleepduration);
    this->sysCtrl->setBatteryInterval(clientconfig->batteryinterval);
    this->sysCtrl->setHostname((char *)clientconfig->name.data, clientconfig->name.len);
    this->sysCtrl->mountLittleFS();
    this->sysCtrl->saveConfiguration();
    this->sysCtrl->unmountLittleFS();

    if (address_changed)
    {
      // No beacon here: CLIENTCONFIG arrives BEFORE CMD_LOGIN, so the counters
      // are still unsynchronised and the hub would reject it.  The login
      // handler sends the beacon once both sides have reset.
      ESP_LOGI(TAG, "Address set to %u by the hub", (unsigned) clientconfig->addr);
      // We have an address now, so stop asking.
      this->cancelRegisterRetry();
    }
    // Counter resets removed: CMD_LOGIN (which always follows CLIENTCONFIG) resets
    // tx_message_id_ and rx_message_id_ to 0.  Resetting here was redundant and,
    // combined with the missing msgid check, caused every burst copy to re-open the
    // replay window for subsequent messages.
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleTimeSync(LoraClientOperationMessage *message_to_process,
                                   const LoraHeader *outer_header)
{
    TimeSync *ts = message_to_process->timesync;
    if (ts == NULL || ts->epoch == 0)
    {
      // epoch 0 means the hub's own clock was not valid — nothing to apply.
      // Leave s_clock_valid alone: a previously good clock is better than none.
      ESP_LOGW(TAG, "   CMD TIMESYNC ignored (no usable epoch)");
      return;
    }

    // Measure the correction BEFORE applying it.  On a re-sync this is the
    // node's accumulated drift, which is the whole point of shipping TimeSync
    // ahead of the scheduler: it lets the crystal be characterised on real
    // hardware against real sleep cycles.
    struct timeval before;
    gettimeofday(&before, NULL);
    const bool     had_clock = s_clock_valid;
    const int64_t  delta_s   = static_cast<int64_t>(ts->epoch) -
                               static_cast<int64_t>(before.tv_sec);

    struct timeval tv;
    tv.tv_sec  = static_cast<time_t>(ts->epoch);
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    s_utc_offset_s = ts->utcoffset;
    s_dst_next     = ts->dstnext;
    s_clock_valid  = true;

    // U-4: the hub's own count of this node's in-slot uplinks. Taken from the
    // hub because it is the only end that can measure it, and stored rather
    // than acted on here — timedRxActive() reads it at every arming decision.
    this->hub_in_slot_uplinks_ = ts->inslotuplinks;

    char local_str[32];
    this->formatLocalTime(ts->epoch, local_str, sizeof(local_str));
    if (had_clock)
    {
      ESP_LOGI(TAG, "   CMD TIMESYNC: %s (UTC%+d) — drift correction %+" PRId64 " s",
               local_str, static_cast<int>(s_utc_offset_s / 3600), delta_s);
    }
    else
    {
      // We have what the retry was asking for.
      this->cancelClockRetry();
      ESP_LOGI(TAG, "   CMD TIMESYNC: %s (UTC%+d) — clock established",
               local_str, static_cast<int>(s_utc_offset_s / 3600));
    }
    // Deliberately NOT acked: config pushes (ClientConfig/CoverConfig) are not
    // acked either, and the wake beacon will carry the node's epoch back to the
    // hub, which is the designed way for the hub to observe drift.  Acking here
    // would spend battery on a redundant transmission.

    // Re-evaluate automatic mode now that we have a clock.
    //
    // The hub sends TimeSync and ScheduleConfig as two separate frames, ~1.25 s
    // apart.  Nothing guarantees they arrive in that order — or at all: observed
    // live, the schedule landed while the TimeSync was still missing, so
    // shouldRunAutoMode() correctly refused (I8: never sleep against a schedule
    // you cannot evaluate) and the node then stayed interactive FOREVER, because
    // nothing ever asked again.
    //
    // Checking here makes the arrival order irrelevant: whichever of the two
    // lands last is the one that starts auto mode.  Queued, not called inline —
    // this is the RX task.
    if (this->shouldRunAutoMode())
    {
      char next_str[32] = "none";
      const uint64_t next = this->computeNextEvent();
      if (next != 0)
        this->formatLocalTime(next, next_str, sizeof(next_str));
      ESP_LOGI(TAG, "Clock established with a schedule already loaded — "
                    "entering automatic mode, next event %s", next_str);
      // Deferred: the ScheduleConfig push follows this frame by ~1.25 s, and
      // sleeping here would miss it and every one of its retransmits.
      this->armAutoSleep();
    }

    // TIER 1: the hub said it has nothing further — sleep NOW instead of
    // waiting out the quiet window.
    //
    // Without this the node cannot distinguish "the hub has finished" from
    // "the hub is slow", so it waits a fixed 20 s of silence. Measured: 73% of
    // a 28.1 s check-in wake is exactly that wait, with all the real work done
    // by 4.7 s. The hub knows the answer -- it has a per-node queue -- so it
    // simply says so.
    //
    // Guarded on the LOCAL state too. The hub only knows what IT has queued;
    // a motor move still running, or interactive mode, means this wake is not
    // over whatever the hub thinks. armAutoSleep() above stays as the fallback
    // for a hub that never sets the flag.
    // C2: tell the Class A sequence first, and UNCONDITIONALLY.
    //
    // sleepOk means the hub's per-node queue is empty, so there is nothing
    // left for RX1 or RX2 to catch. That is true whether or not this node
    // then sleeps — a node held awake by a running motor or by interactive
    // mode still has nothing to listen FOR, and holding the sequence open
    // costs it the shared one-shot for two windows the hub will not transmit
    // in.
    //
    // Passed straight through rather than gated on the local conditions below:
    // those decide whether to SLEEP, which is a different question from
    // whether a reply is still coming.
    this->noteClassASleepOk(ts->sleepok);

    if (ts->sleepok && this->shouldRunAutoMode() && !motCtrl->isBusy())
    {
      // Run any entry that has fallen due FIRST — the same guard autoSleepCb
      // applies before its own sleep, and for the same reason.
      //
      // Skipping it cost a scheduled close on 2026-08-31. The node woke at
      // 21:44:55 for a 21:45:00 event, correctly found "nothing due" 4 s
      // early, then spent 18 s on the beacon/TimeSync exchange. sleepOk
      // arrived at 21:45:14 -- after the event -- and this path slept for the
      // full check-in interval without ever checking. With catchupWindow=0
      // the entry was dropped and the blind stayed open all night.
      //
      // The quiet window used to hide this: it kept the node awake long
      // enough for autoSleepCb to catch the entry. Shortening the wake is
      // exactly what exposed it.
      if (this->runDueScheduleEntry())
        ESP_LOGI(TAG, "   Executed an entry that fell due while awake");

      ESP_LOGI(TAG, "   Hub says sleepOk — sleeping now (quiet window skipped)");
      this->cancelAutoSleep();
      this->setSystemCommand(BlindsSysCmd::SYSCMD_SLEEP);
    }
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
// ---------------------------------------------------------------------------
// Drift test (bench only).
//
// Switched over the air so a test can be started and stopped from HA without
// reflashing. The node holds continuous RX for the duration so it hears most of
// each 17-copy burst and every burst yields a fit.
//
// THE DEADLINE IS THE POINT. Continuous RX is ~11 mA against a ~1.2 mA
// interactive average — roughly 9x. If the "off" command were lost (and it can
// be: it is a single downlink like any other) a node with no timeout would sit
// there until the pack was flat. So the node owns the end of the test, not the
// hub, and the duration is capped regardless of what the hub asks for.
// ---------------------------------------------------------------------------
// A timing sample — see the declaration for why nothing is decoded here.
//
// The frame index is INFERRED from elapsed time rather than read from a header:
//
//     index = round((rx_us - t0) / period)
//
// The hub emits on a known grid, and jitter is microseconds against a period of
// hundreds of milliseconds, so rounding cannot pick the wrong slot. A frame lost
// to the air simply leaves a gap in the index sequence, exactly as a missing
// msgid would -- without needing the frame to be parseable at all.
// Stash the arrival instant. Nothing is committed to the fit here.
//
// The timestamp must be taken at RxDone -- everything after it (queue, task
// wake, SPI FIFO read, decrypt) adds jitter -- but WHICH frame it belongs to
// can only be known after the frame is decoded. So: capture early, commit late.
//
// An earlier version committed every RxDone straight into the fit, on the
// grounds that the test only cares about timing. That folded in acks, schedule
// pushes and CRC-passing noise: 291 "frames" over a 295 s run on an 1100 ms
// grid, when only ~268 were real. The contamination is what left the fit and
// the measured period disagreeing in sign.
void CmdDispatcher::noteDriftSample(int64_t rx_us)
{
  this->drift_rx_us_ = rx_us;
}

void CmdDispatcher::handleDriftTest(LoraClientOperationMessage *message_to_process,
                                    const LoraHeader *outer_header)
{
  const DriftTest *dt = message_to_process->drifttest;
  if (dt == nullptr)
  {
    ESP_LOGE(TAG, "   DriftTest: empty message");
    return;
  }

  // COMMIT a timing sample — this frame is provably a DriftTest.
  //
  // rx_us was captured at RxDone by the ISR, so it carries none of the decode
  // latency; only the ATTRIBUTION happens here. Indexed by msgid, which is
  // exact and gap-tolerant: a frame lost to the air leaves a hole rather than
  // shifting every later sample.
  if (this->drift_test_active_ && dt->enable && outer_header != nullptr &&
      this->drift_rx_us_ != 0 && this->drift_period_ms_ != 0)
  {
    const int64_t period_us = (int64_t) this->drift_period_ms_ * 1000;

    if (!this->drift_have_first_)
    {
      this->drift_first_msgid_ = outer_header->msgid;
      this->drift_have_first_  = true;
    }

    if (outer_header->msgid >= this->drift_first_msgid_)
    {
      const uint32_t idx = outer_header->msgid - this->drift_first_msgid_;
      this->drift_idx_ = idx;
      this->drift_fit_.add((int64_t) idx * period_us, this->drift_rx_us_);

      if (this->drift_fit_.ready() && (this->drift_fit_.n % 20) == 0)
        ESP_LOGW(TAG, "DRIFT: %+d ppm over %u frames, %lld s baseline, "
                      "period %d us (nominal %lld)",
                 (int) this->drift_fit_.ppm(), (unsigned) this->drift_fit_.n,
                 (long long) (this->drift_fit_.span_us() / 1000000),
                 (int) this->drift_fit_.measured_period_us(idx),
                 (long long) period_us);
    }
  }
  if (!dt->enable)
  {
    ESP_LOGW(TAG, "   DriftTest: OFF (by hub)");
    stopDriftTest_();
    return;
  }

  // The NODE owns the duration, not the hub -- see drift::testDurationS.
  const uint32_t secs = drift::testDurationS(dt->durations);
  if (dt->durations != secs)
    ESP_LOGW(TAG, "   DriftTest: hub asked for %u s, using %u s",
             (unsigned) dt->durations, (unsigned) secs);

  // Only clear the accumulator when a test actually STARTS. The hub re-sends
  // this message every grid period (each send is itself the ruler being
  // measured), so a re-arm must add to the average rather than restart it --
  // otherwise the result would only ever reflect the final burst.
  const bool starting = !this->drift_test_active_;
  if (starting)
  {
    // DISABLE light sleep outright for the duration of the test.
    //
    // esp_pm_configure(light_sleep_enable = false) rather than holding an
    // ESP_PM_NO_LIGHT_SLEEP lock: the lock only asks the power manager not to
    // sleep, and leaves automatic light sleep armed underneath. Turning the
    // mechanism off is unambiguous, and it is what makes the measurement
    // trustworthy -- waking from light sleep on a GPIO needs the wake source
    // explicitly armed, so with it enabled the node slept through most of a
    // burst (17 copies became 3-6).
    //
    // Deep sleep is refused separately, in SystemCtrl::enterDeepSleepTask.
    // Between them the node stays awake for the whole baseline, which is the
    // one thing the measurement cannot do without.
    SystemCtrl::applyPowerProfile(true);   // no light sleep, 240 MHz pinned
    ESP_LOGW(TAG, "   DriftTest: light sleep DISABLED, CPU pinned at 240 MHz");
    esp_log_level_set("CmdDispatcher",  ESP_LOG_WARN);
    esp_log_level_set("LoraInterface",  ESP_LOG_WARN);
    esp_log_level_set("frtosTasks",     ESP_LOG_WARN);

    this->drift_acc_.reset();
    this->drift_n_ = 0;
    this->drift_copy0_ref_ = 0;
    this->drift_fit_.reset();
    this->drift_have_first_ = false;
  }
  this->drift_test_active_ = true;
  if (dt->gridperiodms != 0)
    this->drift_period_ms_ = dt->gridperiodms;
  this->loraIf->setContinuousRx(true);

  if (this->drift_test_timer_ == nullptr)
  {
    const esp_timer_create_args_t args = {
        .callback = &CmdDispatcher::driftTestExpiredCb_,
        .arg      = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name     = "drifttest",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&args, &this->drift_test_timer_);
  }
  esp_timer_stop(this->drift_test_timer_);            // restart if already running
  esp_timer_start_once(this->drift_test_timer_, (uint64_t) secs * 1000000);

  if (starting)
    ESP_LOGW(TAG, "   DriftTest: ON for %u s (continuous RX, ~11 mA)", secs);
  else if (this->drift_acc_.ready())
    ESP_LOGW(TAG, "   DriftTest: %+d ppm so far over %u bursts (%u s left)",
             (int) this->drift_acc_.mean_ppm(), (unsigned) this->drift_acc_.count, secs);
}

void CmdDispatcher::stopDriftTest_()
{
  if (!this->drift_test_active_)
    return;
  this->drift_test_active_ = false;
  this->loraIf->setContinuousRx(false);


  // Normal verbosity restored. This runs on every exit path, the node-owned
  // deadline included, so a hub that disappears mid-test cannot leave the node
  // permanently quiet.
  esp_log_level_set("CmdDispatcher",  ESP_LOG_INFO);
  esp_log_level_set("LoraInterface",  ESP_LOG_INFO);
  esp_log_level_set("frtosTasks",     ESP_LOG_INFO);

  // Restore the normal power profile — frequency scaling and automatic light
  // sleep back on. Runs on EVERY exit path, the node-owned deadline included,
  // so a hub that disappears mid-test cannot leave the node burning battery at
  // 240 MHz with sleep disabled.
  SystemCtrl::applyPowerProfile(false);   // scaling + auto light sleep back on
  if (this->drift_test_timer_ != nullptr)
    esp_timer_stop(this->drift_test_timer_);

  if (this->drift_fit_.ready())
    ESP_LOGW(TAG, "   DriftTest RESULT: %+d ppm from %u frames over %lld s "
                  "(measured period %d us)",
             (int) this->drift_fit_.ppm(), (unsigned) this->drift_fit_.n,
             (long long) (this->drift_fit_.span_us() / 1000000),
             (int) this->drift_fit_.measured_period_us(this->drift_idx_));
  else
    ESP_LOGW(TAG, "   DriftTest ended with no usable fit");
}

// Safety net: the node ends its own test.
//
// This is a one-shot esp_timer rather than a poll in one of the task loops
// because every one of those loops blocks on its queue with portMAX_DELAY --
// they only run when traffic arrives. If the hub died mid-test that is exactly
// when nothing would tick, and the node would sit in ~11 mA continuous RX until
// the pack was flat. A timer cannot be starved by silence.
void CmdDispatcher::driftTestExpiredCb_(void *arg)
{
  auto *self = static_cast<CmdDispatcher *>(arg);
  ESP_LOGW(TAG, "   DriftTest: OFF (deadline reached)");
  self->stopDriftTest_();
}

// ---------------------------------------------------------------------------
// ModeTest — the on-hardware validation mode. test-plan.md section 10.
//
// Lifecycle copied from DriftTest, because the two lessons that shape it were
// paid for there: the node owns the deadline (a one-shot esp_timer, because
// every task loop blocks on its queue with portMAX_DELAY and would not tick if
// the hub died), and EVERY exit path restores everything.
//
// What is new here is the third thing to restore. DriftTest only changes the
// power profile and log levels; ModeTest can force a MODE. A node left in
// Mode B against a hub that has forgotten the grid is a node that has stopped
// answering, and nothing about it looks broken from either end.
//
// The decisions — duration cap, arm refusals, commensurate-period rule,
// distribution summarising — are all in ModeTestPolicy.h with their tests.
// This is the wiring.
// ---------------------------------------------------------------------------
void CmdDispatcher::handleModeTest(LoraClientOperationMessage *message_to_process,
                                   const LoraHeader *outer_header,
                                   int64_t rx_us)
{
  ModeTest *mt = message_to_process->modetest;
  if (mt == NULL)
  {
    ESP_LOGW(TAG, "   ModeTest with no payload — ignored");
    return;
  }

  // A running test: this frame IS the ruler. Count it before anything else, so
  // a frame that arrives after the deadline still shows up as received rather
  // than vanishing.
  if (this->mode_test_active_)
  {
    this->mt_seq_.note(mt->seq);
    if (rx_us > 0 && this->grid_.active)
    {
      const int64_t t0 = phase::t0FromRx(rx_us, (uint32_t) this->last_rx_len_);
      // A stamped mark is measured against the instant it declares, so a mark
      // the hub sent late reads as the node's error (zero), not the hub's.
      const bool stamped = (outer_header != nullptr) && outer_header->firestamped;
      const int64_t predicted =
          stamped ? gridstate::t0ForHubInstantUs(this->grid_, outer_header->fireround,
                                                 outer_header->fireoffsetus)
                  : gridstate::nextT0Us(this->grid_, t0 - (int64_t) timedgrid::kRoundUs / 2);
      this->mt_phase_err_.add((int32_t) (t0 - predicted));

      // Residual rate, Mode B marks only (seq 0 is the unplaced START). The
      // prediction already carries the learned rate, so a node that has
      // learned it correctly shows a flat residual. A burst copy is backed
      // out to copy 0 first, as the raw fit does.
      if (this->mt_mode_ == (uint8_t) modetest::Mode::B && mt->seq != 0)
      {
        const uint32_t bi = (outer_header != nullptr && !stamped) ? outer_header->burstindex : 0u;
        const int64_t t0c = t0 - (int64_t) bi * drift::kCopySpacingUs;
        const int64_t pred = stamped ? predicted
                                     : gridstate::nextT0Us(this->grid_, t0c - (int64_t) timedgrid::kRoundUs / 2);
        if (!this->mt_have_first_pred_)
        {
          this->mt_first_pred_us_   = pred;
          this->mt_have_first_pred_ = true;
        }
        this->mt_residual_fit_.add(pred - this->mt_first_pred_us_,
                                   t0c  - this->mt_first_pred_us_);
      }
    }
    this->noteModeTestMarkForRate_(rx_us, outer_header);
  }

  if (!mt->enable)
  {
    ESP_LOGW(TAG, "   ModeTest: OFF (by hub)");
    stopModeTest_();
    return;
  }

  if (this->mode_test_active_)
    return;   // already running; the frames are the test, not a re-arm

  // ARM ONLY ON THE START, which is seq 0. Every mark and the STOP carry
  // seq >= 1, because start_mode_test resets mt_seq_ to 0 immediately before
  // building the START and each later frame post-increments it.
  //
  // MEASURED 2026-09-13: the node ends its test at its own deadline, but the
  // hub keeps sending marks until duration_s + 5 s. A mark landing in that gap
  // found mode_test_active_ false and, with nothing here to tell it from a
  // START, armed a brand-new test — which the hub's STOP then ended ~2 s later.
  // The bench saw it as a second report, `seq 278..278 exp 1, elapsed 2 s`,
  // straight after a genuine Mode A run. In Mode A that is cosmetic; in Mode B
  // a re-armed test rewrites the power profile and timed-RX state for those
  // seconds, on a node whose previous run just ended.
  //
  // Fixed here rather than by trimming the hub's grace period, so a late,
  // duplicated or replayed mark can never begin a test whatever the hub does.
  //
  // AN EARLY MARK MAY START IT, though (modetest::markMayStartTest). Measured
  // 2026-09-14: node 2 missed the whole START burst while still in Mode A and
  // ignored every mark after it, so a 900 s run produced nothing.
  const bool late_start = (mt->seq != 0);
  if (late_start && !modetest::markMayStartTest(mt->seq))
  {
    ESP_LOGW(TAG, "   ModeTest: seq %u with no test running — a late mark, "
                  "not a START; ignored", (unsigned) mt->seq);
    return;
  }

  // ---- Arming ----------------------------------------------------------
  modetest::Request req;
  req.mode = (modetest::Mode) mt->mode;
  req.grid_period_ms = mt->gridperiodms;
  req.copies         = mt->copies;
  req.duration_s     = mt->durations;

  modetest::NodeContext ctx;
  // mac-layer.md §4: the arming frame must itself have arrived authenticated.
  // applyMacConfig_ has always passed this; handleModeTest did not, and then
  // wrote the same two sublayer variables applyMacConfig_ guards.
  ctx.frame_authenticated = this->frame_authenticated_;
  ctx.has_session   = this->session_proven_;
  ctx.is_bench_node = this->bench_node_;
  ctx.has_adopted_grid = this->grid_.active;
  ctx.battery_mv    = (uint32_t) (s_lastBatteryVoltage * 1000.0f);
  ctx.rx_interval_ms = (uint32_t) this->loraIf->rxWindowPeriodMs();
  ctx.rx_window_us   = timedgrid::kWindowUs;

  // A node that has never reported a voltage would otherwise refuse every test
  // for a battery it has not measured. Unknown is not the same as flat.
  if (s_lastBatteryVoltage <= 0.0f)
    ctx.battery_mv = modetest::kMinBatteryMv;

  const modetest::ArmRefusal refusal = modetest::armRefusal(req, ctx);
  this->mt_last_refusal_ = (uint32_t) refusal;
  if (refusal != modetest::ArmRefusal::None)
  {
    ESP_LOGW(TAG, "   ModeTest: REFUSED (%s)", modeTestRefusalName_(refusal));
    // Not from a mark: every heard mark of a refused run would send a report.
    // The START's own refusal, below, is the one the hub reads.
    if (late_start)
      return;
    // REPORT IT. A refusal used to be logged and nothing else, so the hub's
    // `arm refusal` entity — the number the bench procedure says to read
    // FIRST — could never show one: it kept the previous run's 0, and a run
    // that never happened looked like a run whose report was lost. Measured
    // 2026-09-13: three Mode B presses refused for "no adopted grid", visible
    // only on the serial console.
    //
    // Accumulators reset and mode 0 (not run), so the report describes THIS
    // request only. Sent unreset it would attach the refusal to the previous
    // run's mode, counters and histograms — the misreading "read mode actually
    // run first" exists to prevent.
    this->resetModeTestAccumulators_();
    this->mt_mode_ = 0;
    this->sendModeTestReport_();
    return;
  }

  const uint32_t requested_secs = modetest::testDurationS(mt->durations);
  if (mt->durations != requested_secs)
    ESP_LOGW(TAG, "   ModeTest: hub asked for %u s, using %u s",
             (unsigned) mt->durations, (unsigned) requested_secs);
  // Joined late: end with the hub, not a whole duration after this mark.
  const uint32_t secs = modetest::remainingDurationS(requested_secs, mt->seq,
                                                     mt->gridperiodms);
  if (secs == 0)
  {
    ESP_LOGW(TAG, "   ModeTest: seq %u leaves nothing of the run; ignored",
             (unsigned) mt->seq);
    return;
  }
  if (late_start)
    ESP_LOGW(TAG, "   ModeTest: START not heard — joining at seq %u, %u s left",
             (unsigned) mt->seq, (unsigned) secs);

  // ---- Save everything this test can disturb ---------------------------
  //
  // Captured BEFORE anything is changed, and `valid` is what makes the restore
  // on an early exit path a no-op rather than a write of zeroes over the
  // node's real mode and slot.
  this->mt_saved_ = modetest::SavedState{};
  this->mt_saved_.mode        = (uint8_t) (this->timed_rx_enabled_ ? 1 : 0);
  this->mt_saved_.slot        = (uint8_t) this->grid_.params.slot_index;
  this->mt_saved_.continuous_rx = false;
  this->mt_saved_.counter_enabled = this->sublayers_.counter_enabled;
  this->mt_saved_.crypto_enabled  = this->sublayers_.crypto_enabled;
  this->mt_saved_.power_profile_production = true;
  this->mt_saved_.valid = true;

  // MEASURE WITH THE RECEIVE PATH QUIET, as DriftTest does. At 115200 baud an
  // INFO line costs ~10 ms and the RX path prints ~20 of them between the DIO0
  // interrupt and the handler: the first MAC-ping turnaround ever taken on a
  // session-holding node read 82 ms, of which ~70 was the UART. That is the
  // logging, not the MAC, and the servable-slot geometry (HW-7) rests on the
  // MAC. Restored in stopModeTest_, which every exit path runs.
  esp_log_level_set("CmdDispatcher",  ESP_LOG_WARN);
  esp_log_level_set("LoraInterface",  ESP_LOG_WARN);
  esp_log_level_set("frtosTasks",     ESP_LOG_WARN);

  // ---- Apply ------------------------------------------------------------
  // Snapshot the REAL funnel rather than starting a second one.
  //
  // mt_counters_ used to be its own Counters that nothing ever incremented, so
  // every funnel field in the report read zero — the half of the report that
  // matters most, silently empty. A delta against funnel_ cannot drift from
  // the counters the node actually keeps, and "the difference between two runs
  // of the identical grid" is the measurement this mode exists to make anyway.
  this->resetModeTestAccumulators_();

  // The ruler for the run's clock-rate fit. The hub fires marks at this period
  // (Mode A off its timer; Mode B on the grid's round), so it is the nominal
  // spacing each mark index stands for.
  this->mt_grid_period_ms_ = mt->gridperiodms;

  // APPLY THE MODE. This is what the handler never did.
  //
  // mt_mode_ was assigned from the request and stored, and the only other thing
  // the request's mode reached was MODE_SWEEP's arm offset — so the node's RX
  // discipline was whatever it already was, and every "Mode Test B" run was a
  // Mode A measurement wearing a Mode B label. The save above and the restore
  // in stopModeTest_() were both already written for a mode that changes; only
  // the change itself was missing.
  //
  // MODE_UNSPEC leaves it alone, by its own definition on the wire. MODE_C is
  // refused at arm time (modeIsImplemented) and MODE_B/MODE_SWEEP need an
  // adopted grid (modeNeedsGrid), so neither reaches here in a state where
  // setting the flag would strand the node.
  switch ((modetest::Mode) mt->mode)
  {
    case modetest::Mode::A:
      this->timed_rx_enabled_ = false;
      break;
    case modetest::Mode::B:
    case modetest::Mode::Sweep:
      this->timed_rx_enabled_ = true;
      break;
    case modetest::Mode::Unspec:
    case modetest::Mode::C:
    default:
      break;
  }

  // Derived from state, never from the request — see modeApplied.
  this->mt_mode_ = (uint8_t) modetest::modeApplied((modetest::Mode) mt->mode,
                                                   this->timed_rx_enabled_);
  this->mt_power_profile_production_ = !mt->droppowerprofile;
  this->mt_mac_echo_ = mt->macecho;

  // BOTH SUBLAYERS DEFAULT OFF for the duration, unlike production. Turning one
  // on and re-running the identical grid is what makes its cost a measured
  // delta instead of an estimate. Restored on every exit path below.
  this->sublayers_.counter_enabled = mt->enablecounter;
  this->sublayers_.crypto_enabled  = mt->enablecrypto;
  // The hub's ping `seq` restarts with the hub (and with each start_mac_ping), so
  // a high-water carried in from the previous test would refuse every ping.
  this->mac_ping_seq_hw_ = 0;

  // The production power profile is what this mode measures — the node as it
  // ships — and that is now the WIRE default rather than a comment's claim.
  //
  // The field used to be `keepPowerProfile`, documented "DEFAULT TRUE". proto3
  // scalars have no presence, so an omitted field arrived as false and this
  // branch ran: 240 MHz pinned, light sleep off, and a report labelled as if it
  // were a production measurement. Inverting the field makes proto3's own zero
  // the safe answer, so a sender can no longer get it wrong by silence.
  if (mt->droppowerprofile)
  {
    SystemCtrl::applyPowerProfile(true);   // no light sleep, 240 MHz pinned
    ESP_LOGW(TAG, "   ModeTest: power profile DISABLED — results are NOT "
                  "production numbers");
  }

  if (mt->mode == MODE_TEST__MODE__MODE_SWEEP && mt->armoffsetus != 0)
    this->grid_.params.arm_offset_us = mt->armoffsetus;

  this->mode_test_active_ = true;

  if (this->mode_test_timer_ == nullptr)
  {
    const esp_timer_create_args_t args = {
        .callback = &CmdDispatcher::modeTestExpiredCb_,
        .arg      = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name     = "modetest",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&args, &this->mode_test_timer_);
  }
  esp_timer_stop(this->mode_test_timer_);
  esp_timer_start_once(this->mode_test_timer_, (uint64_t) secs * 1000000);

  ESP_LOGW(TAG, "   ModeTest: ON for %u s, mode %u, %u copies, grid %u ms, "
                "counter %d, crypto %d, echo %d, production profile %d",
           (unsigned) secs, (unsigned) mt->mode, (unsigned) mt->copies,
           (unsigned) mt->gridperiodms, (int) mt->enablecounter,
           (int) mt->enablecrypto, (int) mt->macecho,
           (int) !mt->droppowerprofile);

  // The mark that started the run is part of it. Only its sequence is counted:
  // the phase and rate samples at the top of this handler run for marks of a
  // test that was already active.
  if (late_start)
    this->mt_seq_.note(mt->seq);
}

// The report carries RAW COUNTERS, not conclusions.
//
// Nothing here is a rate, a verdict or a summary sentence. The node is the one
// thing that cannot be trusted to judge its own reception — a mode that decided
// it was working would report that it was working — so every rate in
// mac-layer.md section 6 is recomputed off-node from these numbers. That is
// independence claim I1, and it is why this function does no arithmetic beyond
// summarising the timing samples it holds.
// One definition of "a fresh run's accumulators", shared by arming and by the
// refusal report, so the two cannot disagree about what a report starts from.
void CmdDispatcher::resetModeTestAccumulators_()
{
  this->mt_funnel_base_ = this->macFunnelSnapshot();
  this->mt_seq_ = modetest::SeqTracker{};
  this->mt_phase_err_.clear();
  this->mt_arm_residual_.clear();
  this->mt_turnaround_.clear();
  this->mt_one_shot_err_.clear();
  this->mt_started_us_ = nodeclock::nowUs();
  this->mt_drift_fit_.reset();
  this->mt_have_first_t0_ = false;
  this->mt_first_t0_us_   = 0;
  this->mt_last_k_        = 0;
  this->mt_residual_fit_.reset();
  this->mt_have_first_pred_ = false;
  this->mt_first_pred_us_   = 0;
}

// THE MEASUREMENT MODE B EXISTS FOR: the node's clock rate against the hub's,
// under whatever power profile the run applied — production means auto light
// sleep on the 32.768 kHz crystal, which is exactly the clock DriftTest cannot
// see (it runs with sleep disabled).
//
// Nothing fed a rate estimate during a ModeTest before this: drift_fit_ is
// DriftTest's alone, so every ModeTest report carried ppmEstimate = 0.
//
// Ruler: the marks themselves. Each is regressed as T0 against k * period, k
// being the NEAREST whole number of periods since the run's first heard mark.
// Nearest rather than seq-derived, so a mark the hub had to place one round
// later (a busy mark) or a mark this node did not hear leaves a gap instead of
// shifting every later sample; rounding is safe because drift over a run is
// milliseconds against a 750 ms half-period at the 1500 ms round.
//
// Both axes relative to the first mark. LongFit sums squares in doubles, and
// absolute microsecond timestamps (~1e10) would cancel badly in its slope.
//
// A burst copy is backed out to copy 0, the same correction the phase path
// makes: copy N arrives N strides late by design, not by drift.
void CmdDispatcher::noteModeTestMarkForRate_(int64_t rx_us, const LoraHeader *outer_header)
{
  if (!this->mode_test_active_ || rx_us <= 0 || this->mt_grid_period_ms_ == 0)
    return;

  const uint32_t burst_index = (outer_header != nullptr) ? outer_header->burstindex : 0u;
  const int64_t t0 = phase::t0FromRx(rx_us, (uint32_t) this->last_rx_len_)
                   - (int64_t) burst_index * drift::kCopySpacingUs;
  const int64_t period_us = (int64_t) this->mt_grid_period_ms_ * 1000;

  if (!this->mt_have_first_t0_)
  {
    this->mt_first_t0_us_   = t0;
    this->mt_have_first_t0_ = true;
  }
  const int64_t since = t0 - this->mt_first_t0_us_;
  const int64_t k = (since >= 0) ? (since + period_us / 2) / period_us
                                 : -((-since + period_us / 2) / period_us);
  if (k < this->mt_last_k_)
    return;   // a stale or reordered frame; the fit wants monotonic marks
  if (k == this->mt_last_k_ && this->mt_drift_fit_.n > 0)
    return;   // a second copy of the same mark: one sample per mark
  this->mt_last_k_ = k;
  this->mt_drift_fit_.add(k * period_us, since);
}

void CmdDispatcher::sendModeTestReport_()
{
  LoraClientResponseMessage resp = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
  LoraHeader header = LORA_HEADER__INIT;
  header.destaddress   = this->destAddress;
  header.destsubnet    = this->destSubnet;
  header.senderaddress = sysCtrl->getConfigAddress();
  header.msgid         = this->session_.nextTxId();

  const modetest::Hist phase  = this->mt_phase_err_.summarise();
  const modetest::Hist arm    = this->mt_arm_residual_.summarise();
  const modetest::Hist turn   = this->mt_turnaround_.summarise();
  const modetest::Hist oneshot = this->mt_one_shot_err_.summarise();

  Hist h_phase = HIST__INIT, h_arm = HIST__INIT, h_turn = HIST__INIT, h_one = HIST__INIT;
  const modetest::Hist *src[4] = {&phase, &arm, &turn, &oneshot};
  Hist *dst[4] = {&h_phase, &h_arm, &h_turn, &h_one};
  for (int i = 0; i < 4; ++i)
  {
    dst[i]->min = src[i]->min; dst[i]->p50 = src[i]->p50;
    dst[i]->p95 = src[i]->p95; dst[i]->p99 = src[i]->p99;
    dst[i]->max = src[i]->max; dst[i]->n   = src[i]->n;
  }

  ModeTestReport rep = MODE_TEST_REPORT__INIT;
  rep.seqfirst = this->mt_seq_.first;
  rep.seqlast  = this->mt_seq_.last;
  rep.elapseds = (uint32_t) ((nodeclock::nowUs() - this->mt_started_us_) / 1000000);
  rep.mode     = this->mt_mode_;
  rep.powerprofileproduction = this->mt_power_profile_production_;

  // Deltas against the snapshot taken when the test armed. Saturating at zero:
  // the funnel can be reset from elsewhere mid-test (resetMacFunnel), and a
  // wrapped unsigned subtraction would report four billion detected frames
  // rather than the truth, which is that this run's count is unknown.
  const macfunnel::Counters now_c = this->macFunnelSnapshot();
  const macfunnel::Counters &base  = this->mt_funnel_base_;
  auto delta = [](uint32_t a, uint32_t b) -> uint32_t { return (a > b) ? (a - b) : 0u; };

  rep.detected        = delta(now_c.detected,         base.detected);
  rep.crcvalid        = delta(now_c.crc_valid,        base.crc_valid);
  rep.addressed       = delta(now_c.addressed,        base.addressed);
  rep.counteraccepted = delta(now_c.counter_accepted, base.counter_accepted);
  rep.micvalid        = delta(now_c.mic_valid,        base.mic_valid);
  rep.crcerrors       = delta(rep.detected,           rep.crcvalid);
  rep.duplicates      = delta(now_c.duplicates,       base.duplicates);
  rep.micfailures     = delta(now_c.mic_failures,     base.mic_failures);
  rep.windowsarmed    = delta(now_c.windows_armed,    base.windows_armed);
  rep.windowshit      = delta(now_c.windows_hit,      base.windows_hit);
  rep.windowsempty    = delta(rep.windowsarmed,       rep.windowshit);
  rep.seqgaps         = this->mt_seq_.gaps;

  rep.phaseerrus     = &h_phase;
  rep.armresidualus  = &h_arm;
  rep.turnaroundus   = &h_turn;
  rep.oneshoterrorus = &h_one;

  rep.rtcslowsrc = (uint32_t) this->rtc_slow_src_;
  rep.tickratehz = (uint32_t) configTICK_RATE_HZ;
  rep.cpufreqmhz = (uint32_t) (esp_clk_cpu_freq() / 1000000);
  rep.counteron  = this->sublayers_.counter_enabled;
  rep.cryptoon   = this->sublayers_.crypto_enabled;
  rep.armrefusal = this->mt_last_refusal_;
  // Mode B's goal, as numbers. 0 / 0 until three marks have been heard.
  rep.ppmestimate = this->mt_drift_fit_.ppm();
  rep.ppmsamples  = this->mt_drift_fit_.n;
  rep.measuredperiodus = this->mt_drift_fit_.measured_period_us(
      (uint32_t) (this->mt_last_k_ > 0 ? this->mt_last_k_ : 0));
  // The rate the CORRECTED prediction still drifts at. Mode B pass line.
  rep.residualppm     = this->mt_residual_fit_.ppm();
  rep.residualsamples = this->mt_residual_fit_.n;

  resp.header         = &header;
  resp.proto_case     = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MODETESTREPORT;
  resp.modetestreport = &rep;

  uint8_t *out_buf = nullptr;
  size_t   out_len = 0;
  if (!this->pack_response_message(&resp, &out_buf, &out_len))
  {
    ESP_LOGE(TAG, "   ModeTest report: pack failed");
    return;
  }
  const bool ok = this->send_tx_buffer(out_buf, out_len);
  free(out_buf);

  // The serial log is the fallback path, and it matters: the report frame is
  // one uplink on a link the test may just have proved unreliable, so the
  // numbers must also exist somewhere that does not depend on the radio.
  ESP_LOGW(TAG, "   ModeTest REPORT seq %u..%u (%u gaps) detected %u crcValid %u "
                "addressed %u | phaseErr p50 %d p99 %d max %d n %u | "
                "turnaround p50 %d p99 %d n %u | counter %d crypto %d prod %d | tx %d",
           (unsigned) rep.seqfirst, (unsigned) rep.seqlast, (unsigned) rep.seqgaps,
           (unsigned) rep.detected, (unsigned) rep.crcvalid, (unsigned) rep.addressed,
           (int) phase.p50, (int) phase.p99, (int) phase.max, (unsigned) phase.n,
           (int) turn.p50, (int) turn.p99, (unsigned) turn.n,
           (int) rep.counteron, (int) rep.cryptoon,
           (int) rep.powerprofileproduction, (int) ok);
}

const char *CmdDispatcher::modeTestRefusalName_(modetest::ArmRefusal r)
{
  switch (r)
  {
    case modetest::ArmRefusal::None:             return "none";
    case modetest::ArmRefusal::NotAuthenticated: return "arming frame was not authenticated";
    case modetest::ArmRefusal::NoSession:        return "no session";
    case modetest::ArmRefusal::SweepOffBench:    return "MODE_SWEEP off the bench";
    case modetest::ArmRefusal::NoGrid:           return "MODE_B/MODE_SWEEP with no adopted grid";
    case modetest::ArmRefusal::ModeUnimplemented: return "MODE_C is not a mode this test can apply";
    case modetest::ArmRefusal::CommensurateGrid: return "grid period phase-locks with the RX interval";
    case modetest::ArmRefusal::BatteryTooLow:    return "battery too low";
    case modetest::ArmRefusal::BadCopies:        return "copies outside 1..17";
    case modetest::ArmRefusal::NoGridPeriod:     return "no grid period";
  }
  return "unknown";
}

void CmdDispatcher::stopModeTest_()
{
  if (!this->mode_test_active_)
    return;
  this->mode_test_active_ = false;

  sendModeTestReport_();

  // Verbosity back, unconditionally and before anything that could return: a hub
  // that disappears mid-test (this runs from the node-owned deadline too) must
  // not leave the node mute.
  esp_log_level_set("CmdDispatcher",  ESP_LOG_INFO);
  esp_log_level_set("LoraInterface",  ESP_LOG_INFO);
  esp_log_level_set("frtosTasks",     ESP_LOG_INFO);

  // Restore EVERYTHING, on every exit path including the node-owned deadline.
  // The mode and slot are the ones DriftTest never had to think about: a node
  // left in Mode B against a hub that has forgotten the grid stops answering,
  // and looks healthy while it does.
  if (this->mt_saved_.valid)
  {
    this->sublayers_.counter_enabled = this->mt_saved_.counter_enabled;
    this->sublayers_.crypto_enabled  = this->mt_saved_.crypto_enabled;
    this->timed_rx_enabled_        = (this->mt_saved_.mode != 0);
    this->grid_.params.slot_index  = this->mt_saved_.slot;
    this->grid_.params.arm_offset_us = 0;   // a sweep offset never survives
    this->loraIf->setContinuousRx(this->mt_saved_.continuous_rx);
    SystemCtrl::applyPowerProfile(false);   // scaling + auto light sleep back on
    this->mt_saved_.valid = false;
  }

  if (this->mode_test_timer_ != nullptr)
    esp_timer_stop(this->mode_test_timer_);

  ESP_LOGW(TAG, "   ModeTest: OFF, everything restored");
}

// The node ends its own test. Same reasoning as driftTestExpiredCb_: a timer
// cannot be starved by silence, and silence is exactly what a dead hub gives.
void CmdDispatcher::modeTestExpiredCb_(void *arg)
{
  auto *self = static_cast<CmdDispatcher *>(arg);
  ESP_LOGW(TAG, "   ModeTest: deadline reached");
  self->stopModeTest_();
}

void CmdDispatcher::handleSchedule(LoraClientOperationMessage *message_to_process,
                                   const LoraHeader *outer_header)
{
    ScheduleConfig *sc = message_to_process->schedule;
    if (sc == NULL)
    {
      ESP_LOGW(TAG, "   CMD SCHEDULE with no payload — ignored");
      return;
    }

    sched::Entry entries[sched::kMaxEntries];
    uint8_t count = 0;
    for (size_t i = 0; i < sc->n_entries && count < sched::kMaxEntries; i++)
    {
      const ScheduleEntry *pe = sc->entries[i];
      if (pe == NULL)
        continue;
      sched::Entry e;
      e.minuteOfDay = (uint16_t) pe->minuteofday;
      e.dayMask     = (uint8_t)  pe->daymask;
      e.action      = (uint8_t)  pe->action;
      e.positionPct = (uint8_t)  pe->positionpct;
      // The hub only sends entries it wants active, so anything that arrives is
      // enabled by definition.
      e.enabled     = true;
      entries[count++] = e;
    }

    // A schedule the node already holds is not written to flash again. The hub
    // re-sends a ScheduleConfig until it hears the ack, so the same schedule
    // arrives several times; every copy rewrote config.txt. Measured 2026-09-15
    // on node 2 (fw 1.0.85): the reset that emptied config.txt came during the
    // save of such a repeat, 3 s after the first copy had been saved. The
    // version is the hub's hash of the entries; the scalar fields are compared
    // too (a zero beacon lead / post-event window means "keep", as in setSchedule).
    const bool unchanged =
        sc->version != 0 &&
        this->sysCtrl->getSchedVersion() == sc->version &&
        this->sysCtrl->getAutoMode() == (sc->mode != 0) &&
        this->sysCtrl->getInteractiveTimeout() == sc->interactivetimeout_s &&
        this->sysCtrl->getCheckinInterval() == sc->checkininterval_s &&
        this->sysCtrl->getCatchupWindow() == sc->catchupwindow_s &&
        (sc->beaconlead_s == 0 || this->sysCtrl->getBeaconLead() == sc->beaconlead_s) &&
        (sc->posteventwindow_s == 0 || this->sysCtrl->getPostEventWindow() == sc->posteventwindow_s);
    if (unchanged)
    {
      ESP_LOGI(TAG, "   CMD SCHEDULE: version=0x%08x already held — not rewritten",
               (unsigned) sc->version);
    }
    else
    {
      this->sysCtrl->setSchedule(sc->version, (uint8_t) sc->mode,
                                 sc->interactivetimeout_s, sc->checkininterval_s,
                                 sc->beaconlead_s, sc->posteventwindow_s,
                                 sc->catchupwindow_s, entries, count);

      this->sysCtrl->mountLittleFS();
      this->sysCtrl->saveConfiguration();
      this->sysCtrl->unmountLittleFS();
    }

    ESP_LOGI(TAG, "   CMD SCHEDULE: version=0x%08x mode=%s entries=%u",
             (unsigned) sc->version,
             sc->mode == NODE_MODE__MODE_AUTO ? "AUTO" : "INTERACTIVE",
             (unsigned) count);

    // Acked, unlike TimeSync: the hub retransmits a schedule push until it is
    // acknowledged, and it needs to know its pending config actually landed
    // before it clears the "config pending" state.
    this->sendCommandAck(outer_header->msgid);

    // Enter automatic mode by QUEUEING a sleep, never by calling
    // enterDeepsleep() from here.
    //
    // Calling it directly from this (the RX/dispatcher) task crashed the node
    // within ~1 s of the schedule arriving — panic-class reset, observed live.
    // Tearing the radio down from inside the receive path is not safe.
    //
    // SYSCMD_SLEEP routes through processSysCommand's own task, which is
    // exactly how the nightly CMD_SLEEP has always worked in production. Same
    // destination, a context that is known to survive it.
    if (this->shouldRunAutoMode())
    {
      char next_str[32] = "none";
      const uint64_t next = this->computeNextEvent();
      if (next != 0)
        this->formatLocalTime(next, next_str, sizeof(next_str));
      ESP_LOGI(TAG, "Schedule applied — entering automatic mode, next event %s", next_str);
      // Deferred so our CommandAck is actually sent and the hub has a quiet
      // window to follow up in before the radio goes down.
      this->armAutoSleep();
    }
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleLogin(LoraClientOperationMessage *message_to_process,
                                const LoraHeader *outer_header)
{
    // F-30: Rate-limit LoginMsg to prevent a rogue transmitter from rapidly
    //       resetting the frame counter.
    // The window is per dispatcher, not per process: it used to be a
    // function-static, which is the same thing on the node (one dispatcher)
    // but leaks across gtest cases in the host harness — one test's login
    // silently rate-limited the next test's, so a security test looked like it
    // passed while measuring the limiter. Member state, reset with the object.
    static const uint64_t LOGIN_RATE_LIMIT_MS = 5000; // 5 seconds
    uint64_t now_ms = nodeclock::nowUs() / 1000ULL;
    if (this->last_login_ms_ != 0 &&
        (now_ms - this->last_login_ms_) < LOGIN_RATE_LIMIT_MS)
    {
      ESP_LOGW(TAG, "LoginMsg rate-limited: arrived %" PRIu64 " ms after last one (min %llu ms)",
               (now_ms - this->last_login_ms_), LOGIN_RATE_LIMIT_MS);
      return;
    }
    this->last_login_ms_ = now_ms;

    LoginMsg *login = message_to_process->login;

    // Config-sync: the hub sets request_register when it has NOT pushed config to
    // us this session (e.g. it rebooted for a config change / OTA while we stayed
    // awake and re-established via LOGIN instead of REGISTER).  Respond by
    // REGISTERing: that drives the hub's normal register -> config-push -> login
    // path, so an awake node picks up the new config without rebooting.
    //   * sendRegister() (SYSCMD_REGISTER) resets our counters and clears the base
    //     nonce itself, so we skip the nonce-store / login-ack below and let the
    //     fresh login that follows the config push (request_register=false) install
    //     a new nonce and complete the handshake normally.
    //   * The follow-up login arrives ~kRegisterToLoginDelayMs (4 s) + round-trip
    //     later; if it lands inside our 5 s login rate-limit window it is dropped,
    //     but a dropped login does not update last_login_ms_, so the hub's 5 s
    //     login retry then lands outside the window and is accepted.  Converges.
    if (login->request_register)
    {
      this->destAddress = outer_header->senderaddress;
      this->destSubnet  = outer_header->destsubnet;
      ESP_LOGI(TAG, "CMD_LOGIN from peer %u requests re-register (hub config out of sync) — sending REGISTER",
               (unsigned)outer_header->senderaddress);
      this->sendRegister();
      return;
    }

    // Reset both message-ID counters to zero on every login.
    this->session_.resetCounters();

    // The LoginMsg.nonce field carries the hub's AES-GCM base-nonce.
    // Storing it here means CMD_LOGIN + nonce provisioning are handled in a
    // single message — no separate CMD_BASENONCE needed for the login sequence.
    // CMD_BASENONCE remains available as a standalone recovery path only.
    this->destAddress = outer_header->senderaddress;
  // F-5: the peer we persist for IS the hub we just established a session with.
  //
  // SessionManager's persist peer defaulted to 0xFF and was only ever assigned
  // from a blob it
  // had just loaded, so on a node whose blob said 255 it stayed 255 forever
  // while the live nonce was filed under the hub's real address (1). Two
  // consequences, both silent:
  //
  //   * the persist-on-login below never fired (1 != 255), so a fresh nonce was
  //     never written; and
  //   * savePersistentState() looked up get_base_nonce(255), found an ancient
  //     broadcast-keyed entry, and rewrote THAT on every sleep.
  //
  // So the node restored the same stale state on every boot —
  //     F-5: restored state hub=255 base_nonce=0x20fc0cdd tx=390(+64) rx=0
  // — and its resume beacon was encrypted with a nonce the hub had never held
  // and a msgid 450 ahead of the hub's counter. The hub answered
  // psa_aead_decrypt failed: -149, the resume fallback fired, and the node
  // re-registered. The beacon-first resume could never have worked.
  this->session_.setPersistPeer(outer_header->senderaddress);
    set_base_nonce(outer_header->senderaddress, login->nonce);
    this->destSubnet  = outer_header->destsubnet;

    ESP_LOGI(TAG, "CMD_LOGIN from peer %u — counters reset, base_nonce=0x%08x",
             (unsigned)outer_header->senderaddress, (unsigned)login->nonce);

    // Both sides have just reset their counters, so this is the first moment a
    // beacon can actually be received.  Sending it here (rather than at boot)
    // is what makes the hub's TimeSync + schedule push happen on a register.
    this->sendWakeBeacon(this->wake_reason_);

    // Acknowledge the login so the hub cancels its retry interval.
    // assigns msgid=1 (first post-reset tx), encrypts with the freshly stored
    // base_nonce, and sends it.  The hub's set_response() sees msgid=1 > rx=0,
    // sets login_acked_=true, and cancels the hourly retry interval.
    this->sendAvailable();
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleBaseNonce(LoraClientOperationMessage *message_to_process,
                                    const LoraHeader *outer_header)
{
    BaseNonceExchange *exchange = message_to_process->basenonce;
    if (exchange && exchange->base_nonce.len == 4)
    {
      uint32_t base_nonce = 0;
      base_nonce |= exchange->base_nonce.data[0];
      base_nonce <<= 8;
      base_nonce |= exchange->base_nonce.data[1];
      base_nonce <<= 8;
      base_nonce |= exchange->base_nonce.data[2];
      base_nonce <<= 8;
      base_nonce |= exchange->base_nonce.data[3];
      // Same reasoning as the CMD_LOGIN path above: file the persisted state
      // under the peer we actually have a session with.
      this->session_.setPersistPeer(outer_header->senderaddress);
      set_base_nonce(outer_header->senderaddress, base_nonce);
      // No separate frame counters to reset — the nonce is derived from
      // the LoraHeader msgid, which is the single unified counter.
      ESP_LOGI(TAG, "Stored base nonce 0x%08x for peer %u", base_nonce, outer_header->senderaddress);
      this->destAddress = outer_header->senderaddress;
      this->destSubnet = outer_header->destsubnet;
    }
    else
    {
      ESP_LOGE(TAG, "Invalid base nonce exchange payload");
    }
}

// Extracted verbatim from the onReceiveNew switch. Each handler is entered
// only after admitFrame() has accepted the frame, so none of them repeats
// the address / replay / encryption checks.
void CmdDispatcher::handleCoverConfig(LoraClientOperationMessage *message_to_process,
                                      const LoraHeader *outer_header)
{
    ESP_LOGI(TAG, "   CMD CONFIG");

    CoverConfig *coverconfig = message_to_process->coverconfig;

    // Travel durations: 0 means "not set", exactly as the roll geometry below
    // and batteryInterval in ClientConfig already treat it. proto3 cannot
    // distinguish an absent uint32 from a zero one, and a zero is not a slow
    // blind — it is the absence of a duration.
    //
    // MEASURED 2026-09-22 (fw 1.1.4). Node 2's config.txt held
    // "motorOpenDuration":0,"motorCloseDuration":0 and every move was instant:
    //
    //   MOTCMD_FULL_DOWN -> Current position 1.000000 -> 0.000000  (~1 s moving)
    //
    // runMsForMove() returns the configured duration for a full move, so a zero
    // fires the move timer immediately and the FULLY_* TIMER transition SNAPS
    // the position to an extreme. The blind twitched through the LEDC stop fade
    // while the reported position teleported to fully closed; on the other node
    // the same zero made a stop at 80 % report "closed". These two fields were
    // the only ones in CoverConfig applied unconditionally.
    const uint32_t open_s =
        motorpolicy::keptTravelDurationS(coverconfig->opentime,
                                         this->sysCtrl->getConfigMotorOpenDuration());
    const uint32_t close_s =
        motorpolicy::keptTravelDurationS(coverconfig->closetime,
                                         this->sysCtrl->getConfigMotorCloseDuration());

    if (coverconfig->opentime == 0u || coverconfig->closetime == 0u)
      ESP_LOGW(TAG, "CoverConfig carried an unset duration (open=%u close=%u) — "
                    "keeping open=%u close=%u",
               (unsigned) coverconfig->opentime, (unsigned) coverconfig->closetime,
               (unsigned) open_s, (unsigned) close_s);

    ESP_LOGI(TAG, "Setting new openduration: %u", (unsigned) open_s);
    ESP_LOGI(TAG, "Setting new closeduration: %u", (unsigned) close_s);
    this->sysCtrl->setTimes(open_s, close_s);
    // Push the (travel-only) durations to the motor controller immediately so a
    // runtime CoverConfig takes effect this session, not only after the next boot.
    this->motCtrl->setRuntime(open_s, close_s);

    // Slat-slack at the bottom end (un-seal head / seal tail).  Applied both to
    // the persisted config and live to the motor controller.  Zero = disabled.
    ESP_LOGI(TAG, "Setting slack: openSlack=%d s, closeSlack=%d s",
             (unsigned int)coverconfig->openslack, (unsigned int)coverconfig->closeslack);
    this->sysCtrl->setSlack(coverconfig->openslack, coverconfig->closeslack);
    this->motCtrl->setSlack(coverconfig->openslack, coverconfig->closeslack);

    // Apply roll geometry if the hub provided non-zero values.
    // Zero means "not set" in proto3 — keep firmware defaults in that case.
    if (coverconfig->blindheightmm > 0.0f &&
        coverconfig->axlediametermm > 0.0f &&
        coverconfig->blindthicknessmm > 0.0f)
    {
      ESP_LOGI(TAG, "Setting roll geometry: height=%.1f mm, axle=%.1f mm, thickness=%.2f mm",
               coverconfig->blindheightmm, coverconfig->axlediametermm, coverconfig->blindthicknessmm);
      this->sysCtrl->setGeometry(coverconfig->blindheightmm,
                                 coverconfig->axlediametermm,
                                 coverconfig->blindthicknessmm);
      this->motCtrl->setRollGeometry(coverconfig->axlediametermm,
                                     coverconfig->blindthicknessmm,
                                     coverconfig->blindheightmm);
    }

    this->sysCtrl->mountLittleFS();
    this->sysCtrl->saveConfiguration();
    this->sysCtrl->unmountLittleFS();
}

// ---------------------------------------------------------------------------
// MAC-0: the MAC control frame.
//
// Counted, timestamped, optionally echoed — and never handed to the
// application. See configuration/docs/mac-layer.md sections 1 and 5.
//
// The echo goes straight to the radio TX queue via send_tx_buffer(), NOT
// through setStatus() and the sys/tx command queues that every application
// reply uses. That is the entire point: routing it through the application
// would put application dispatch inside the turnaround being measured, and the
// servable-slot geometry budgets zero for it.
// ---------------------------------------------------------------------------
// Arm or disarm the MAC-1 / MAC-2 sublayers. MAC control frames only; see
// MacSublayers.h for why the scope is narrow and why arming is authenticated
// even though the traffic being measured is not.
void CmdDispatcher::applyMacConfig_(const MacControl *mc)
{
  const bool has_session   = this->canResumeSession();
  // Negative sense on the wire so that a zeroed message is the SAFE one.
  const bool wants_degrade = mc->disablecounter || mc->disablecrypto;

  const macsublayers::ArmRefusal refusal = macsublayers::armRefusal(
      has_session, this->frame_authenticated_,
      this->bench_node_, wants_degrade);

  if (refusal != macsublayers::ArmRefusal::None)
  {
    // Reported, never silently ignored: a dropped test command is otherwise
    // indistinguishable from a lost one, and the operator would keep resending.
    ESP_LOGW(TAG, "   MAC config REFUSED (reason %d, session=%d, authenticated=%d)",
             (int) refusal, (int) has_session, (int) this->frame_authenticated_);
    return;
  }

  this->sublayers_.counter_enabled = !mc->disablecounter;
  this->sublayers_.crypto_enabled  = !mc->disablecrypto;

  const uint32_t secs = macsublayers::disableDurationS(mc->durations);
  if (mc->durations != secs)
    ESP_LOGW(TAG, "   MAC config: hub asked for %u s, using %u s",
             (unsigned) mc->durations, (unsigned) secs);

  ESP_LOGW(TAG, "   MAC config: MAC-1 %s, MAC-2 %s for %u s (control frames only)",
           this->sublayers_.counter_enabled ? "on" : "OFF",
           this->sublayers_.crypto_enabled ? "on" : "OFF", (unsigned) secs);

  if (this->sublayer_restore_timer_ == nullptr)
  {
    const esp_timer_create_args_t args = {
        .callback              = &CmdDispatcher::sublayerRestoreCb_,
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "mac_sublayer",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &this->sublayer_restore_timer_) != ESP_OK)
    {
      // Could not arm the deadline, so do NOT take the degraded path: restore
      // immediately. Losing a measurement beats a node parked with a sublayer
      // off because the hub went away.
      ESP_LOGE(TAG, "   MAC config: no restore timer — reverting now");
      this->sublayers_ = macsublayers::Config{};
      return;
    }
  }
  esp_timer_stop(this->sublayer_restore_timer_);
  esp_timer_start_once(this->sublayer_restore_timer_, (uint64_t) secs * 1000000ULL);
}

void CmdDispatcher::sublayerRestoreCb_(void *arg)
{
  auto *self = static_cast<CmdDispatcher *>(arg);
  if (self == nullptr)
    return;
  self->sublayers_ = macsublayers::Config{};
  ESP_LOGW(TAG, "   MAC config: deadline reached — MAC-1 and MAC-2 restored");
}

void CmdDispatcher::handleMacControl(LoraClientOperationMessage *message_to_process,
                                     const LoraHeader *outer_header,
                                     int64_t rx_us)
{
  const MacControl *mc = message_to_process->maccontrol;
  if (mc == nullptr)
  {
    ESP_LOGE(TAG, "   MacControl: empty message");
    return;
  }

  if (mc->kind == MAC_CONTROL__KIND__MAC_CONFIG)
  {
    this->applyMacConfig_(mc);
    return;
  }

  // Only a PING is actionable on the node. An echo arriving here is either our
  // own frame looped back or a peer misbehaving; count nothing and reply to
  // nothing, or two nodes could echo each other indefinitely.
  if (mc->kind != MAC_CONTROL__KIND__MAC_PING)
  {
    ESP_LOGW(TAG, "   MacControl: ignoring kind %d", (int) mc->kind);
    return;
  }

  this->mac_.ping_rx++;
  this->mac_.last_seq = mc->seq;
  // drift_rx_us_ is the RxDone timestamp captured by the ISR, before any
  // decode latency — the same sample the drift test uses.
  // THIS FRAME's arrival instant, passed down rather than read from
  // drift_rx_us_. That member is written by the DIO0 task too, so the next
  // burst copy landing 88 ms later could overwrite it between dispatch and
  // here — producing a turnaround short by 88 ms, or negative. It is the one
  // number the servable-slot geometry rests on.
  this->mac_.last_rx_us = rx_us;

  ESP_LOGI(TAG, "   MAC ping seq=%u len_pad=%u%s", (unsigned) mc->seq,
           (unsigned) mc->pad.len, mc->wantecho ? " (echo)" : "");

  // A running ModeTest with macEcho off silences the echo even when the frame
  // asks for one: the run is measuring the node WITHOUT a reply in the path,
  // and answering anyway would put the echo's own transmit back into the
  // interval being measured.
  if (this->mode_test_active_ && !this->mt_mac_echo_)
    return;

  if (!mc->wantecho)
    return;

  // Build the echo. Same seq, so the hub can join the two sides on the mark
  // rather than on arrival order.
  LoraClientResponseMessage resp = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
  LoraHeader header = LORA_HEADER__INIT;
  // Same fields, from the same sources, as every other uplink this class
  // builds (see processTxCommand) — the echo is an ordinary frame on the wire.
  header.destaddress   = this->destAddress;
  header.destsubnet    = this->destSubnet;
  header.senderaddress = sysCtrl->getConfigAddress();
  header.msgid         = this->session_.nextTxId();
  (void) outer_header;

  MacControl echo = MAC_CONTROL__INIT;
  echo.kind     = MAC_CONTROL__KIND__MAC_ECHO;
  echo.seq      = mc->seq;
  echo.wantecho = false;

  resp.header     = &header;
  resp.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
  resp.maccontrol = &echo;

  uint8_t *out_buf = nullptr;
  size_t   out_len = 0;
  if (!this->pack_response_message(&resp, &out_buf, &out_len))
  {
    this->mac_.echo_failed++;
    ESP_LOGE(TAG, "   MAC echo: pack failed");
    return;
  }

  // Timestamp as late as possible before handing the bytes over, so the
  // recorded turnaround contains the work rather than excluding it.
  const int64_t fire_us = nodeclock::nowUs();
  const bool ok = this->send_tx_buffer(out_buf, out_len);
  free(out_buf);

  if (!ok)
  {
    this->mac_.echo_failed++;
    ESP_LOGW(TAG, "   MAC echo: TX queue full");
    return;
  }

  this->mac_.echo_tx++;
  this->mac_.last_echo_fire_us = fire_us;
  if (this->mac_.last_rx_us != 0)
  {
    this->mac_.last_turnaround_us = fire_us - this->mac_.last_rx_us;
    // Section 12.7's open measurement, which the plan's servable-slot rule
    // rests on and which nothing recorded a distribution for. RxDone -> TX
    // fire, and nothing else: a MAC echo runs no application dispatch, which
    // is exactly why turnaround measured any other way is not this number.
    // Inert unless a ModeTest is running.
    this->noteModeTestTurnaround((int32_t) this->mac_.last_turnaround_us);
  }
}

// ---------------------------------------------------------------------------
// B3: adopt (or withdraw) the timed-window grid.
//
// The grid is MAC-layer state and terminates here: nothing in this path
// reaches a cover, a schedule or the motor.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// B3: the single-window decision.
//
// Expressed through TimedModePolicy so the node and the hub reason about
// promotion with the same rules rather than two copies that drift.
// ---------------------------------------------------------------------------
bool CmdDispatcher::timedRxActive() const
{
  // THE EDGE IS DETECTED BEFORE THE EARLY RETURN, deliberately.
  //
  // This comparison sat BELOW `if (!timed_rx_enabled_) return false;` until
  // 2026-09-21, which made the one transition the hub most needs
  // unannounceable: when the grid is withdrawn the node's reason becomes
  // GridDisabled, this function returns immediately, and the reason is never
  // compared — so the flag is never raised and the hub is never told. It also
  // left last_reason_announced_ stale across any off/on cycle, so the hub went
  // on believing a reason the node had already left.
  //
  // MEASURED on node 2 that day: Timed Mode withdrawn, 120 s of hub log
  // captured, not one beacon, and the hub's demotion-reason sensor still
  // republishing its previous value. The announce fired at boot (grid enabled)
  // and never again, which is exactly the shape this ordering produces.
  //
  // Safe to evaluate here: demotionReasonNow() returns GridDisabled for this
  // very case as its own first line, so the announced reason is true rather
  // than a guess, and the call costs one compare when timed RX is off.
  const uint8_t reason = this->demotionReasonNow();

  // TELL THE HUB, not just the console. Keyed on what the hub was last told
  // rather than the log's own field: the log fires once per change and the wire
  // must fire once per change THE HUB HAS NOT BEEN TOLD ABOUT, which are
  // different whenever the floor suppressed an announcement. Only the flag is
  // raised here — this is a const accessor the receive task calls on every
  // arming pass, and transmitting from it would put a queue push behind a
  // getter.
  //
  // s_last_reason_announced is an RTC_DATA_ATTR static, so it SURVIVES a
  // deep-sleep wake. As a plain member it was reset by every wake, every reason
  // then compared unequal to 0xFF, and the node announced on each wake before
  // sending its check-in beacon anyway — two uplinks for one fact.
  if (reason != s_last_reason_announced)
    this->mode_announce_pending_ = true;

  if (!this->timed_rx_enabled_)
    return false;

  // SAY WHY, once per change. Measured 2026-09-14 on node 2: a run caught 11
  // frames after grid adoption and never promoted, and nothing on the console
  // said which rule held it back or what the phase statistics looked like — the
  // answer could only be guessed at from the next run. The receive task asks
  // this on every pass, so a line per call would flood; a line per change is the
  // whole history in a handful of lines.
  if (reason != this->last_reason_logged_)
  {
    this->last_reason_logged_ = reason;
    const phase::Stats &s = this->phase_;
    ESP_LOGW(TAG, "   timed RX: reason %u | phase n %u frames %u outside %u "
                  "min %d max %d last %d us",
             (unsigned) reason, (unsigned) s.n, (unsigned) s.frames,
             (unsigned) s.outside_guard, s.n ? (int) s.min_us : 0,
             s.n ? (int) s.max_us : 0, (int) s.last_us);
  }

  // The edge detection used to sit HERE as well. When it was hoisted above the
  // early return on 2026-09-21 the original was left behind, so the function
  // carried the rule twice — the "two copies that drift" failure this file
  // warns about elsewhere, and one of them unreachable in exactly the
  // !timed_rx_enabled_ case the hoist existed to fix. One copy, above.

  return reason == (uint8_t) timedmode::Demotion::None;
}

// Tell the hub our Mode B status changed, at most once per
// timedmode::kModeAnnounceMinS.
//
// THE LAST STATE WINS, deliberately. The announcement carries the reason as it
// is NOW, not the one that raised the flag: a node that flickered 0 -> 5 -> 0
// inside the floor must leave the hub believing 0. Recording the flag-raising
// reason instead would describe a transition that no longer holds, which is the
// same "a field that quietly lies" failure this project keeps paying for.
//
// The floor is applied to the ANNOUNCEMENT, not to the edge detection, so a
// change that is suppressed now stays pending and goes out at the next pass
// past the floor rather than being lost.
void CmdDispatcher::announceModeChangeIfPending()
{
  if (!this->mode_announce_pending_)
    return;

  const int64_t now_us = nodeclock::nowUs();
  const uint32_t s_since =
      (s_last_mode_announce_us == 0)
          ? 0xFFFFFFFFu
          : (uint32_t) ((now_us - s_last_mode_announce_us) / 1000000);

  if (!timedmode::shouldAnnounceModeChange(true, s_since))
    return;   // still pending: the next pass past the floor sends it

  const uint8_t reason = this->demotionReasonNow();
  this->mode_announce_pending_ = false;
  s_last_reason_announced      = reason;
  s_last_mode_announce_us      = (now_us > 0) ? now_us : 1;

  ESP_LOGW(TAG, "   timed RX: announcing mode change to the hub (reason %u) — "
                "the PhaseReport rides this beacon",
           (unsigned) reason);
  this->setStatus(BlindsStatusCmd::SYSCMD_BEACON,
                  (uint32_t) WAKE_REASON__WAKE_MODE_CHANGED);
}

// Why the node is not in Mode B (Demotion value), or None. Reported to the hub
// in every PhaseReport: the node decides, the hub follows.
uint8_t CmdDispatcher::demotionReasonNow() const
{
  if (!this->timed_rx_enabled_)
    return (uint8_t) timedmode::Demotion::GridDisabled;

  timedmode::NodeState ns;
  ns.grid_enabled             = this->grid_.active;
  ns.rtc_src                  = (this->rtc_slow_src_ == phase::RtcSlowSrc::Crystal)
                                    ? timedmode::RtcSlowSrc::Crystal
                                    : timedmode::RtcSlowSrc::Unknown;
  ns.phase_valid              = phase::phaseTrustworthy(this->phase_, timedgrid::kGuardUs);
  ns.phase_err_us             = this->phase_.last_us;
  // Thin is not wrong: see timedmode::NodeState::in_mode_b. The previous decision
  // is what timedRxActive() last logged, which is exactly "was it in Mode B".
  ns.phase_consistent         = (this->phase_.outside_guard == 0) &&
                                (this->phase_.n == 0 ||
                                 (uint32_t) this->phase_.spread_us() <= timedgrid::kGuardUs);
  ns.in_mode_b                = (this->last_reason_logged_ == (uint8_t) timedmode::Demotion::None);
  // These three were hardcoded, and each hardcode disabled a criterion §4.6
  // treats as load-bearing:
  //
  //   s_since_addressed_frame = 0 meant Demotion::SyncStale could NEVER fire,
  //   so resyncMaxS — carried on the wire, validated, stored — had no effect on
  //   the node at all. A node whose hub went quiet held its phase forever.
  //
  //   in_slot_uplinks was hardcoded to the optimistic value, so
  //   Demotion::NotConfirmed could never fire and §4.6's "an uplink OBSERVED
  //   IN ITS SLOT, not a claim" was not a criterion at all. It comes from the
  //   hub on TimeSync now (U-4), and it HAS to: the placement is produced by
  //   this node's TRANSMIT path and the question is where the frame ARRIVED,
  //   so a locally measured value would be exactly the self-claim §4.6
  //   rejects. [2026-09-14: RETIRED as a criterion — the node is the
  //   authority on its own promotion and a ModeTest sends no uplinks, so the
  //   count could never be earned on the bench. Still carried and stored as a
  //   diagnostic; see TimedModePolicy.h demotionReason.]
  const int64_t now_us = nodeclock::nowUs();
  ns.s_since_addressed_frame =
      (this->last_addressed_us_ == 0)
          ? 0xFFFFFFFFu
          : (uint32_t) ((now_us - this->last_addressed_us_) / 1000000);
  ns.in_slot_uplinks          = this->hub_in_slot_uplinks_;

  return (uint8_t) timedmode::demotionReason(ns, this->grid_.params.resync_max_s,
                                             timedgrid::kGuardUs);
}

// Section 4.3, the node half: aim this uplink at the instant the hub told us to
// reply at, rather than "whenever CAD says the channel is clear".
//
// ulOffsetUs was published, validated and stored and never once READ, so every
// uplink went out after an unconditional random 29-290 ms backoff and the hub's
// in-slot measurement could not succeed by anything but luck. This is the
// function that closes that: the TX task asks for a CAD start instant, and gets
// one whenever the node is genuinely on the grid.
//
// Everything that makes the aim WRONG is refused here rather than approximated:
//
//   * no grid, or no offset published — there is no mark to aim at;
//   * timed RX off — the node is in Mode A and its slot means nothing;
//   * phase not trustworthy — the anchor is the thing the aim is measured from.
//     Aiming off an anchor we do not believe is worse than not aiming: a random
//     backoff lands somewhere harmless, a confident wrong instant lands on
//     another node's slot.
//
// Returns 0 for "not aimed", which the caller reads as "send the old way".
int64_t CmdDispatcher::uplinkCadStartUs(int64_t now_us, int64_t max_wait_us)
{
  if (!this->grid_.active || !this->timed_rx_enabled_)
    return 0;
  if (!phase::phaseTrustworthy(this->phase_, timedgrid::kGuardUs))
    return 0;

  const gridstate::UplinkAim aim =
      gridstate::aimUplink(this->grid_, now_us, kUplinkAimLeadUs, max_wait_us);
  if (!aim.aimed)
  {
    // Counted, not logged. A node whose turnaround exceeds the published offset
    // misses EVERY mark, and that is HW-7's number showing itself in the field
    // — the one place it can be seen without a scope. A silent fallback would
    // look exactly like the offset working.
    if (this->ul_aim_misses_ < 0xFFFFFFFFu)
      this->ul_aim_misses_++;
    return 0;
  }

  if (this->ul_aim_hits_ < 0xFFFFFFFFu)
    this->ul_aim_hits_++;
  return aim.cad_start_us;
}

int64_t CmdDispatcher::nextArmInstantUs(int64_t now_us) const
{
  if (!this->grid_.active)
    return now_us;
  return gridstate::armInstantUs(this->grid_,
                                 gridstate::nextT0Us(this->grid_, now_us));
}

int64_t CmdDispatcher::nextArmDelayUs(int64_t now_us, int64_t lead_us,
                                      gridstate::WindowKind &kind_out,
                                      int64_t opened_t0_us, int64_t *t0_out) const
{
  // Two kinds of window now: this node's private mark, and section 4.4's
  // broadcast beacon. The caller needs to know which, because a beacon window
  // that closes empty is not a missed MARK — the hub promised this node nothing
  // in it — and counting it would make WMR a measure of beacon reception and
  // feed a demotion the node has not earned. Same distinction, and the same
  // reason, as C2's Class A windows.
  //
  // The skip permission applies to the private window only. See
  // gridstate::nextWindow: a node that skipped the beacon as well could never
  // hear the thing that grants and revokes the permission.
  //
  // Asked about the window the search will actually land on — past the one
  // already open — not about the round `now` sits in.
  const int64_t from_us = gridstate::nextWindowSearchFromUs(now_us, opened_t0_us);
  const bool skip_own = !this->shouldArmNextWindow(from_us);
  return gridstate::nextWindowArmDelayUs(this->grid_, now_us, lead_us,
                                         skip_own, kind_out, opened_t0_us, t0_out);
}

// Whether the window for the NEXT mark is worth opening at all.
//
// An armed window is ~29 ms of receive at ~11 mA against a ~1.2 mA average, and
// on a 32-node grid most windows on most nodes are empty. If the last beacon
// said the hub has nothing for this node, and that statement has not expired,
// the window can be skipped.
//
// Every path that is not certain returns true. See PendingData.h: the failure
// mode here is silent — a node that skips wrongly does not report anything, it
// simply stops hearing the hub.
// ---------------------------------------------------------------------------
// C2 — Class A receive windows, placed from the node's own uplink.
//
// Mode C needs no clock agreement with the hub at all: the windows hang off
// T0_uplink, which the node measures on its own clock from its own TxDone. That
// independence is the mode's whole appeal, and it is why C2 could be built
// before the hub's RX timestamp is anywhere near the +/-1 ms it eventually
// wants — the NODE's side does not depend on the hub's precision.
//
// What it does depend on is the node knowing when its own frame left. That was
// unavailable until this session: the synchronous transmit path polled TX_DONE
// at 20 ms granularity, against windows 29.44 ms wide. It is 500 us now, and
// the async path's DIO0 edge is stamped by the ISR.
//
// The sequencing itself lives in ClassAWindows.h with its tests. This is the
// state around it.
// ---------------------------------------------------------------------------
void CmdDispatcher::noteUplinkSent(int64_t t_txdone_us, uint32_t uplink_len)
{
  // Mode C only. This used to activate on EVERY uplink from every node,
  // including one sitting on the hub's grid and one that is awake and
  // interactive, so a mode nobody had selected took over the shared one-shot
  // timer and placed the node's windows off its own transmit instead of off
  // the hub's marks. Two conditions, both necessary:
  //
  //   * automatic mode — an interactive node is awake with the free-running
  //     Mode A window, which is wider and already open; Class A exists to buy
  //     a sleeping node a reply without a clock agreement.
  //   * no timed grid — Mode B's marks ARE the schedule. Letting both drive
  //     the one-shot means whichever ran last wins, and the node's mark rate
  //     collapses for reasons no beacon field explains.
  // The grid test is grid_.active, not timedRxActive(): a node that has
  // ADOPTED a grid is on Mode B even during the interval where its phase is not
  // yet trustworthy and the arming has fallen back to Mode A. Class A jumping
  // into that gap is exactly the mode fight this gate exists to prevent.
  // Mode C wake clock: the RTC tick count at this beacon's T0. Read here, in
  // task context, and walked back to T0 — rtc_time_get() busy-waits for a
  // slow-clock edge and does not belong in the ISR that stamped TxDone. The ISR
  // stamp is node time, so "now" must be node time too.
  if (this->beacon_tx_pending_)
  {
    this->beacon_tx_pending_ = false;
    const uint64_t ticks_now = nodeclock::rtcTicks();
    if (ticks_now != 0 && t_txdone_us > 0)
    {
      s_beacon_t0_ticks = beaconticks::ticksAtT0(ticks_now, nodeclock::nowUs(), t_txdone_us,
                                                 loratiming::t0ToRxDoneUs(uplink_len),
                                                 nodeclock::periodQ19());
      s_beacon_msgid    = this->beacon_tx_pending_msgid_;
    }
  }

  const bool auto_mode = (this->sysCtrl != nullptr) && this->sysCtrl->getAutoMode();
  if (!auto_mode || this->grid_.active)
  {
    portENTER_CRITICAL(&this->classa_mux_);
    this->classa_.active = false;
    portEXIT_CRITICAL(&this->classa_mux_);
    return;
  }

  if (t_txdone_us <= 0)
  {
    // No timestamp for this transmit, so there is no origin to hang windows
    // off. Fall back to the free-running pattern rather than placing windows
    // against a guess — a window in the wrong place is worse than no window,
    // because the node sleeps afterwards believing it listened.
    portENTER_CRITICAL(&this->classa_mux_);
    this->classa_.active = false;
    portEXIT_CRITICAL(&this->classa_mux_);
    return;
  }

  const int64_t t0 = classa::t0UplinkUs(t_txdone_us, uplink_len);
  portENTER_CRITICAL(&this->classa_mux_);
  this->classa_.active       = true;
  this->classa_.t0_uplink_us = t0;
  this->classa_.state        = classa::WakeState{};
  portEXIT_CRITICAL(&this->classa_mux_);
}

// sleepOk (C1) short-circuits the whole sequence: the hub has said its per-node
// queue is empty, so there is nothing to wait for. Proto3 zero must mean "keep
// waiting" — today's behaviour — so a node that has not been told anything does
// not sleep early.
void CmdDispatcher::noteClassASleepOk(bool ok)
{
  // UNDER THE MUTEX, like every other writer of classa_. This function wrote
  // the struct bare while it sat uncalled, and classa_ is cross-core by
  // construction: noteUplinkSent and noteClassAWindowResult run on the DIO0
  // interrupt task, and this runs on the receive task, out of handleTimeSync.
  // The header says as much three lines above the member.
  portENTER_CRITICAL(&this->classa_mux_);
  this->classa_.state.sleep_ok = ok;

  // AND THE INVARIANT: "active" means there is a window still to open.
  //
  // §11a recorded that wiring this would break that invariant. It does not —
  // the invariant says what to do. sleep_ok is the second way for nextAction()
  // to return Sleep, so it needs the same clearing rule noteClassAWindowResult
  // already applies for the first. Left active, the node would hold the shared
  // one-shot for a sequence that has no window left, and armNextRxWindow would
  // keep taking the Class A branch and rely on a delay of 0 to fall through to
  // the mode that actually applies — which is precisely the "both worked, and
  // neither was true" state the invariant was introduced to end.
  //
  // A stale flag cannot leak into the next sequence: noteUplinkSent assigns a
  // fresh WakeState for every uplink.
  if (classa::nextAction(this->classa_.state) == classa::WakeAction::Sleep)
    this->classa_.active = false;
  portEXIT_CRITICAL(&this->classa_mux_);
}

void CmdDispatcher::noteClassAWindowResult(bool had_data, int64_t rx_us)
{
  portENTER_CRITICAL(&this->classa_mux_);
  if (!this->classa_.active)
  {
    portEXIT_CRITICAL(&this->classa_mux_);
    return;
  }
  // Which window this result closes, and the origin it was placed from, taken
  // under the lock for the log line below.
  const int     window_no = this->classa_.state.rx1_done ? 2 : 1;
  const int64_t t0_uplink = this->classa_.t0_uplink_us;
  if (!this->classa_.state.rx1_done)
  {
    this->classa_.state.rx1_done     = true;
    this->classa_.state.rx1_had_data = had_data;
  }
  else
  {
    this->classa_.state.rx2_done = true;
  }

  // The sequence is over: stop being active.
  //
  // It used to stay active until the next uplink, which meant every Mode A
  // window that received anything, for however many hours, was still reported
  // into a finished sequence — and armNextRxWindow kept taking the Class A
  // branch first, relying on a delay of 0 to fall through to the mode that
  // actually applied. Both worked, and neither was true. "Active" now means
  // there is a window still to open.
  if (classa::nextAction(this->classa_.state) == classa::WakeAction::Sleep)
    this->classa_.active = false;
  portEXIT_CRITICAL(&this->classa_mux_);

  // Mode C's window funnel. The sequence above was advanced and nothing
  // counted it: every auto-mode wake reported windows 0 / hits 0 while its RX
  // windows plainly opened (measured 2026-09-14 on node 2, three check-ins).
  // Counted after the Class A lock is released — the funnel has its own.
  //
  // "Hit" here is what the radio saw in a Class A window: a frame arrived
  // (RxDone), whether or not it later turned out to be addressed to this node.
  // Timed windows count their hit at the address filter instead; the Class A
  // result is reported from the radio event, before any of that is known.
  this->noteWindowResult(had_data);

  // Which window, and for a frame how long after this node's own uplink T0 it
  // completed. RX1 is placed at T0_uplink + 1 s, RX2 at + 2 s, so the offset
  // says directly whether a reply landed in the window it was aimed at. Added
  // because every Mode C wake counted two windows and one hit while the node log
  // showed a single reply at the RX1 instant: an RX1 that closed empty just
  // before a late reply would look exactly like that, and nothing at INFO level
  // said which window closed when.
  if (had_data && rx_us > 0)
    ESP_LOGI(TAG, "Class A RX%d: frame, RxDone %+lld us after T0_uplink",
             window_no, (long long) (rx_us - t0_uplink));
  else
    ESP_LOGI(TAG, "Class A RX%d: %s", window_no, had_data ? "frame" : "empty (timeout)");
}

classa::WakeAction CmdDispatcher::classAAction() const
{
  const ClassAState s = this->classaSnapshot_();
  if (!s.active)
    return classa::WakeAction::Sleep;
  return classa::nextAction(s.state);
}

// When the next Class A window should be ARMED, i.e. already accounting for the
// preamble and guard that kArmLeadUs covers. Returns 0 when there is no window
// to open, which the caller must treat as "nothing to schedule" and not as
// "now".
int64_t CmdDispatcher::classAArmInstantUs() const
{
  // ONE snapshot for the whole decision. Reading active, then state, then
  // t0_uplink_us as three separate touches lets a transmit landing between
  // them pair an old action with a new origin.
  const ClassAState s = this->classaSnapshot_();
  if (!s.active)
    return 0;
  switch (classa::nextAction(s.state))
  {
    case classa::WakeAction::OpenRx1:
      return classa::rx1OpenUs(s.t0_uplink_us);
    case classa::WakeAction::OpenRx2:
      return classa::rx2OpenUs(s.t0_uplink_us);
    case classa::WakeAction::Sleep:
    default:
      return 0;
  }
}

// How long to wait before arming, given the caller's own arm lead. Same
// clamping rule as the grid's: never zero or negative, because a window whose
// instant has already passed is still worth opening — lateness inside the guard
// band is survivable and silence is not.
void CmdDispatcher::classAPendingWindow(int64_t *arm_us, int64_t *close_us) const
{
  int64_t arm = 0, close = 0;
  const ClassAState s = this->classaSnapshot_();
  if (s.active)
  {
    switch (classa::nextAction(s.state))
    {
      case classa::WakeAction::OpenRx1:
        arm   = classa::rx1OpenUs(s.t0_uplink_us);
        close = classa::rx1CloseUs(s.t0_uplink_us);
        break;
      case classa::WakeAction::OpenRx2:
        arm   = classa::rx2OpenUs(s.t0_uplink_us);
        close = classa::rx2CloseUs(s.t0_uplink_us);
        break;
      case classa::WakeAction::Sleep:
      default:
        break;
    }
  }
  if (arm_us) *arm_us = arm;
  if (close_us) *close_us = close;
}

int64_t CmdDispatcher::classAArmDelayUs(int64_t now_us, int64_t lead_us) const
{
  const int64_t arm = this->classAArmInstantUs();
  if (arm == 0)
    return 0;
  const int64_t d = arm - lead_us - now_us;
  return (d < 1) ? 1 : d;
}

bool CmdDispatcher::shouldArmNextWindow(int64_t now_us) const
{
  if (!this->grid_.active)
    return true;
  const int64_t t0 = gridstate::nextT0Us(this->grid_, now_us);
  const int64_t rel = t0 - this->grid_.anchor_us;
  if (rel < 0)
    return true;
  const uint32_t round = (uint32_t) (rel / (int64_t) this->grid_.params.round_us);
  return pending::shouldArmWindow(this->pending_,
                                  this->grid_.params.slot_index, round);
}

void CmdDispatcher::noteMarkOutcome(bool addressed)
{
  // Kept for callers that know both halves at once. The two-part form
  // (noteMarkArmed / noteMarkHit / noteMarkMissed) is what the radio paths use,
  // because arming and the outcome happen at different moments and a frame can
  // die in between.
  this->noteWindowResult(addressed);
  if (addressed)
    this->missed_marks_ = 0;
  else
    this->missed_marks_++;
}

// demoteIfMarksMissed_() stood here until 2026-09-20.
//
// It demoted the node after kMaxMissedMarks empty marks, reset the phase
// evidence, and stamped last_demotion_us_ for the 600 s anti-flap hold. All
// three are retired together — see the Demotion banner in TimedModePolicy.h:
// the criterion measured HUB SILENCE, not link health, so 4.5 s of ordinary
// quiet demoted a healthy node, and the hold existed only to rate-limit the
// resulting flap.
//
// TWO SIDE EFFECTS WENT WITH IT, and neither is missed:
//
//   phase_.reset() — a SyncStale demotion no longer throws the evidence away,
//   so when a beacon clears the staleness the node returns to Mode B on the
//   samples it already has instead of re-earning eight. That is faster
//   recovery, and the samples were never wrong: the anchor they were measured
//   against is the one the beacon just confirmed.
//
//   requestRearm() — the receive task re-evaluates on its own next pass
//   (~470 ms) because timedRxActive() is recomputed every time it arms. There
//   was never a state change here that the task could not see for itself.
//
// The grid is still KEPT on every path, which was this function's other job
// (fw 1.0.89): nothing clears grid_.active now except the hub withdrawing it
// by GridSync(enable=false) or GridDemote.

// The hub's startup withdrawal: drop back to Mode A.
//
// Unauthenticated, by design, and this is the whole justification: Mode A is
// three receive windows per round against Mode B's one, so the frame can only
// make this node listen MORE. That is the same rule the beacon exemption rests
// on, and a demote satisfies it more cleanly than a beacon does — a beacon can
// also nudge the anchor, while this can only throw one away.
//
// Two things it deliberately does NOT do.
//
// It does not clear the fleet key. A node with no key accepts UNSIGNED beacons
// again — bounded per beacon, but the guard bounds ONE nudge and not a
// sequence, so anything in radio range could walk the anchor away 14 ms at a
// time. Letting an unauthenticated frame strip the key would hand that attack
// back for the price of one packet. Keeping a stale key costs nothing: the
// netKeyId check already refuses a beacon signed under a different generation,
// so a key that outlived its grid is inert rather than dangerous, and a node in
// Mode A is not using beacon phase for anything.
//
// It is not accepted addressed to this node. A broadcast is what the hub
// actually sends — after a restart it does not yet know which nodes exist — and
// requiring it means an attacker cannot pick one node out of the fleet.
void CmdDispatcher::handleGridDemote(const LoraHeader *outer_header)
{
  const bool broadcast = (outer_header != nullptr) &&
                         (outer_header->destaddress == loraIf->broadcastAddressing);
  if (!broadcast)
  {
    ESP_LOGW(TAG, "   GridDemote: not a broadcast — ignored");
    return;
  }

  if (!this->grid_.active)
    return;   // already in Mode A; every node in range hears this frame

  ESP_LOGW(TAG, "   GridDemote: grid WITHDRAWN by the hub — back to Mode A");
  this->grid_.clear();
  this->phase_.reset();
  this->expected_t0_us_ = 0;
  if (this->loraIf != nullptr)
    this->loraIf->requestRearm();   // back to the free-running window now
  // net_key_ / net_key_id_ are deliberately left alone. See the banner.
}

// Section 4.4. Is this beacon from our hub?
//
// AES-CMAC under the fleet key, and psa_mac_verify rather than compute-then-
// memcmp: the library compares in constant time, and a timing oracle on a MAC
// comparison is the classic way to forge one tag byte at a time.
//
// The truncated tag on the air is 8 bytes. PSA verifies a truncated MAC through
// PSA_ALG_TRUNCATED_MAC, which is the same construction the AEAD already uses
// for its shortened tag — asking it to verify 8 bytes against the full
// algorithm would simply fail on length.
bool CmdDispatcher::beaconMacIsValid(uint32_t net_key_id, uint32_t tx_round,
                                     uint32_t tx_slot, uint32_t pending_mask,
                                     bool pending_mask_valid,
                                     const uint8_t *mac, size_t mac_len) const
{
  if (!this->hasNetKey())
    return false;
  // A beacon signed under a key we do not hold. The hub has rotated and this
  // node has not yet had the GridSync carrying the new one; refusing is right,
  // and the node then coasts on resyncMaxS until its next addressed frame.
  if (net_key_id != this->net_key_id_)
    return false;
  if (mac == nullptr || mac_len != framecrypto::kBeaconMacBytes)
    return false;

  uint8_t input[framecrypto::kBeaconMacInputBytes];
  framecrypto::buildBeaconMacInput(net_key_id, tx_round, tx_slot, pending_mask,
                                   pending_mask_valid, input);

  const psa_algorithm_t alg =
      PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, framecrypto::kBeaconMacBytes);

  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_VERIFY_MESSAGE);
  psa_set_key_algorithm(&attrs, alg);
  psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attrs, framecrypto::kNetKeyBytes * 8);
  psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);

  // Imported per beacon rather than held in a slot. A beacon is one frame every
  // 350 s, so the import is free, and a slot held across a key rotation would
  // verify the new hub's beacons with the old hub's key — silently.
  psa_key_id_t kid = PSA_KEY_ID_NULL;
  psa_status_t st = psa_import_key(&attrs, this->net_key_,
                                   sizeof(this->net_key_), &kid);
  if (st != PSA_SUCCESS)
  {
    ESP_LOGE(TAG, "   beacon MAC: psa_import_key failed: %d", (int) st);
    return false;
  }

  st = psa_mac_verify(kid, alg, input, sizeof(input), mac, mac_len);
  psa_destroy_key(kid);
  return st == PSA_SUCCESS;
}

void CmdDispatcher::handleGridSync(LoraClientOperationMessage *message_to_process,
                                   const LoraHeader *outer_header, int64_t rx_us)
{
  const GridSync *gs = message_to_process->gridsync;
  if (gs == nullptr)
  {
    ESP_LOGE(TAG, "   GridSync: empty message");
    return;
  }

  if (!gs->enable)
  {
    // Withdrawal is unconditional and needs no agreement check: dropping to
    // Mode A is always safe, and the hub sends this on every startup because
    // its anchor is gone while every node still holds the old one.
    if (this->grid_.active)
      ESP_LOGW(TAG, "   GridSync: grid WITHDRAWN — back to Mode A");
    this->grid_.clear();
    this->phase_.reset();
    this->expected_t0_us_ = 0;
    if (this->loraIf != nullptr)
      this->loraIf->requestRearm();   // back to the free-running window now
    // THE KEY STAYS, and the earlier reasoning here was wrong.
    //
    // It read "a key that outlived its grid would let a stale beacon look
    // valid". It would not: the netKeyId check refuses a beacon signed under a
    // different generation, so a stale key is inert. What clearing it actually
    // does is put the node back on the NO-KEY policy, where unsigned beacons
    // are accepted — and the guard band bounds one anchor nudge, not a
    // sequence of them, so that is the walk the key was added to close. A
    // withdrawal must not be a way to re-open it.
    return;
  }

  gridstate::Params p;
  p.slot_index          = gs->slotindex;
  p.slot_count          = gs->slotcount;
  p.round_us            = gs->roundus;
  p.pitch_us            = gs->pitchus;
  p.beacon_slot         = gs->beaconslotindex;
  p.beacon_every_rounds = gs->beaconeveryrounds;
  p.sym_timeout         = gs->symtimeout;
  p.resync_max_s        = gs->resyncmaxs;
  p.ul_offset_us        = gs->uloffsetus;
  p.arm_offset_us       = gs->armoffsetus;

  const gridstate::Refusal r =
      gridstate::validate(p, gs->txslot, this->bench_node_);
  this->grid_refusal_ = r;
  if (r != gridstate::Refusal::None)
  {
    // Refused, not half-adopted. A node that adopted a grid it disagrees with
    // would miss every window and look like a dead radio.
    ESP_LOGE(TAG, "   GridSync REFUSED (reason %d): slots %u pitch %u round %u",
             (int) r, (unsigned) p.slot_count, (unsigned) p.pitch_us,
             (unsigned) p.round_us);
    this->grid_.clear();
    this->expected_t0_us_ = 0;
    return;
  }

  // Solve for a LOCAL anchor from this frame's declared grid position. No
  // clock crosses the link, only a position.
  const int64_t t0_measured =
      phase::t0FromRx(rx_us, (uint32_t) this->last_rx_len_);

  // The declaration describes COPY 0, and a GridSync is a burst: 17 copies, one
  // burst stride apart. The node adopts from whichever copy it happens to
  // decode first — in Mode A it is sweeping a free-running window, so that is
  // rarely copy 0 — and solving the anchor from a later copy's T0 displaces
  // every mark the node will ever arm by that copy's offset. The stride is not
  // a multiple of the slot pitch (88000 vs 46875), so the error is not even a
  // whole number of slots.
  //
  // Back out this copy's own offset first. burstindex is re-stamped per copy by
  // the hub and is already used exactly this way for the drift estimate
  // (noteDriftSample), so the correction costs nothing new on the wire.
  const uint32_t burst_index =
      (outer_header != nullptr) ? outer_header->burstindex : 0u;
  const int64_t t0_copy0 =
      t0_measured - (int64_t) burst_index * drift::kCopySpacingUs;

  // Section 4.4's fleet key, adopted ONLY from an authenticated frame.
  //
  // GridSync is encrypted whenever a session exists and plaintext otherwise, so
  // this gate is what stops a bootstrap frame — or anything in radio range
  // imitating one — from installing the key the whole beacon scheme rests on.
  // A plaintext GridSync that carried a key would make the MAC decorative.
  //
  // Adopted BEFORE the grid is marked active, so there is no window in which a
  // node is on the grid holding the previous grid's key.
  if (this->frame_authenticated_ && gs->netkey.len == framecrypto::kNetKeyBytes
      && gs->netkeyid != 0)
  {
    memcpy(this->net_key_, gs->netkey.data, framecrypto::kNetKeyBytes);
    this->net_key_id_ = gs->netkeyid;
    ESP_LOGI(TAG, "   GridSync: fleet key adopted (id %08x)",
             (unsigned) gs->netkeyid);
  }
  else if (this->net_key_id_ != 0)
  {
    // A grid published without a key, by a hub that has one no longer or never
    // had one. Forgetting is the safe direction: a key that outlived its grid
    // would authenticate beacons for an anchor that no longer exists, and the
    // no-key policy below is exactly what shipped before this existed.
    memset(this->net_key_, 0, sizeof(this->net_key_));
    this->net_key_id_ = 0;
    ESP_LOGW(TAG, "   GridSync: no fleet key on this grid — beacons unsigned");
  }

  this->grid_.active     = true;
  this->grid_.params     = p;
  // The learned clock rate survives re-adoption: it describes this node clock,
  // and a hub reboot or a new slot does not change that. The anchor is solved
  // with it so the grid it seeds predicts on the node clock from the first mark.
  this->grid_.rate_ppb   = this->clock_rate_ppb_;
  // A stamped copy says where it really went out, which is what the anchor has
  // to agree with; the placement (txround/txslot) is only what was intended.
  // Measured 2026-09-14: GridSyncs that left ~320 ms and ~10 ms after their mark
  // moved every mark the node armed by exactly that.
  this->grid_.anchor_us  =
      (outer_header != nullptr && outer_header->firestamped)
          ? gridstate::solveAnchorFromHubInstantUs(t0_measured, outer_header->fireround,
                                                   outer_header->fireoffsetus, p,
                                                   this->clock_rate_ppb_)
          : gridstate::solveAnchorUs(t0_copy0, gs->txround, gs->txslot, p,
                                     this->clock_rate_ppb_);
  this->grid_.last_round = gs->txround;
  // A new anchor starts a new rate span. A stamped GridSync on a settled clock put
  // the anchor exactly where the hub declares it went out, so that instant IS the
  // span's start and the FIRST beacon learns the rate. Measured 2026-09-15 on node
  // 2 (fw 1.0.76): with the span left to the first beacon, the rate came only at
  // the second, 12 min after the grid, and phase drifted +10 ppm to 5.3 ms first.
  // An unstamped or boot-minute anchor carries its own error into the span, so it
  // still waits for a beacon.
  const bool gs_ref = outer_header != nullptr && outer_header->firestamped
                   && nodeclock::settled();
  this->have_rate_ref_   = gs_ref;
  this->rate_ref_hub_us_ = gs_ref ? gridstate::hubInstantUs(p, outer_header->fireround,
                                                            outer_header->fireoffsetus)
                                  : 0;

  // The pending-data bitmap, if this GridSync carried one. `valid` comes from
  // the explicit flag, never from the bits: a zeroed field is indistinguishable
  // from a deliberate "nothing for anyone", and reading a MISSING bitmap as
  // all-clear would put the whole fleet to sleep on the first beacon that
  // omitted it. Absent means listen.
  this->pending_.bits                = gs->pendingmask;
  this->pending_.valid               = gs->pendingmaskvalid;
  this->pending_.beacon_round        = gs->txround;
  this->pending_.beacon_every_rounds = p.beacon_every_rounds;

  // Phase measurement starts from a clean slate: samples taken against the
  // previous anchor describe a grid that no longer exists.
  this->phase_.reset();
  this->noteAnchorFixed_();
  this->expected_t0_us_ = gridstate::nextT0Us(this->grid_, rx_us);
  // A NEW anchor has to earn promotion again: nothing measured it yet. Forget the
  // previous decision so the thin-evidence allowance does not carry over to it.
  this->last_reason_logged_ = 0xFF;
  // Adopted before node time settled: keep the grid (it is what makes the node
  // listen at its marks at all) but trust neither its anchor nor samples against
  // it until a stamped frame on a settled clock re-solves it.
  this->grid_provisional_ = !nodeclock::settled();
  if (this->grid_provisional_)
    ESP_LOGW(TAG, "   GridSync: node clock not settled yet — anchor provisional");

  // Adopting a grid is what enables timed RX.
  //
  // setTimedRxEnabled() had no caller anywhere, so timed_rx_enabled_ was
  // permanently false and timedRxActive() returned false BEFORE consulting the
  // grid at all — a node could accept a GridSync, solve its anchor, and still
  // never arm a timed window. The hub asking for a grid IS the trigger; there
  // is no second thing to wait for, and a separate switch would only be
  // another symbol with no caller.
  //
  // The mode policy still has the final say: timedRxActive() runs the full
  // promotion test (ppm validity, phase trustworthiness, missed marks, RTC
  // source), so enabling this makes Mode B REACHABLE, not active.
  this->timed_rx_enabled_ = true;

  if (p.arm_offset_us != 0)
    ESP_LOGW(TAG, "   GridSync: SWEEP arm offset %+d us — reception is "
                  "deliberately degraded (HW-2)", (int) p.arm_offset_us);

  ESP_LOGW(TAG, "   GridSync: slot %u/%u, anchor %lld us, next T0 %lld us, "
                "beacon slot %u every %u rounds",
           (unsigned) p.slot_index, (unsigned) p.slot_count,
           (long long) this->grid_.anchor_us,
           (long long) this->expected_t0_us_,
           (unsigned) p.beacon_slot, (unsigned) p.beacon_every_rounds);

  // A new anchor and a new pending mask: re-arm against them now.
  if (this->loraIf != nullptr)
    this->loraIf->requestRearm();

  // Confirm the adoption. The hub re-publishes a GridSync until this arrives.
  // Measured 2026-09-15 on node 2: the one GridSync sent after a login was lost
  // while the node spent the burst in CAD retries, nothing re-sent it, and the
  // node sat in Mode A for 13 minutes. Once per GridSync: the burst's other
  // copies carry the same msgid and stop at the replay check. A REFUSED GridSync
  // returned above and is not confirmed.
  if (outer_header != nullptr)
    this->sendCommandAck(outer_header->msgid);
}

// ---------------------------------------------------------------------------
// Section 4.4 — the broadcast beacon.
//
// This is what keeps a node IN Mode B. Without it a node holds phase only for
// resyncMaxS after each addressed frame, and at 3.5 commands/day that is 2.9 %
// of the day on the grid and no measurable battery saving; the mode would be a
// narrower window for nothing.
//
// Three things happen here and the order matters:
//
//   1. The frame is TIMED against where this node expected the beacon to be.
//      That error is drift, and correcting the anchor by it is the whole
//      mechanism — a node that only reset a staleness timer would keep the
//      accumulated error and eventually miss its own window.
//   2. The correction is BOUNDED by the guard band, and stays bounded even for
//      an AUTHENTICATED beacon. The MAC says the frame came from our hub; it
//      says nothing about the hub being right, and any compromised node holds
//      the same fleet key. Real drift is orders of magnitude smaller than the
//      bound anyway — one round at +/-20 ppm is 30 us against a 14080 us guard
//      — so the clamp costs nothing and removes a whole class of outcome.
//   3. The pending bitmap is adopted only from a frame that has proved BOTH
//      that it is on our grid and that it came from our hub. Timing alone is
//      not enough: a mask is an instruction to stop listening, and a stranger
//      who can hear the grid can also transmit on it.
//
// Deliberately NOT done here: adopting geometry. A beacon carries no slot
// assignment, no pitch, no cadence — those are GridSync's, and a broadcast that
// could rewrite them would hand every node in the fleet the same slot.
void CmdDispatcher::handleGridBeacon(LoraClientOperationMessage *message_to_process,
                                     const LoraHeader *outer_header, int64_t rx_us)
{
  const GridBeacon *gb = (message_to_process != nullptr)
                       ? message_to_process->gridbeacon : nullptr;
  if (gb == nullptr)
    return;

  // A node with no grid has no anchor to correct and no window to skip. It is
  // not an error: every node in range hears every beacon.
  if (!this->grid_.active)
    return;

  // Not our beacon slot means not our beacon. The hub publishes the slot in
  // GridSync; a frame claiming a different one is either a stale beacon from
  // before a re-assignment or another grid entirely.
  if (gb->txslot != this->grid_.params.beacon_slot)
  {
    ESP_LOGW(TAG, "   GridBeacon: slot %u is not our beacon slot %u — ignored",
             (unsigned) gb->txslot,
             (unsigned) this->grid_.params.beacon_slot);
    return;
  }

  if (rx_us <= 0)
    return;

  // Section 4.4. Is this beacon ours to act on at all?
  //
  // The ratchet, and it is deliberately a ratchet rather than a flag:
  //
  //   NO KEY HELD -> proceed exactly as before. A beacon may correct the anchor
  //     within the guard band and may never touch the pending mask. A node that
  //     never receives a key therefore does not regress, which is what makes
  //     this safe to ship ahead of the hubs.
  //
  //   A KEY HELD -> the beacon must carry a matching id and a valid MAC, or it
  //     is ignored ENTIRELY: no re-anchor, no phase sample, no mask, no
  //     staleness reset. Tolerating an unsigned beacon once forgery is
  //     detectable would leave the whole attack open, because an attacker would
  //     simply omit the MAC. And the guard band does not save us there: it
  //     bounds ONE nudge, not a sequence, so anything in radio range could walk
  //     a node 14 ms per beacon until it is off the grid.
  //
  // Refusing costs a node nothing worse than the behaviour it had before the
  // beacon existed: it coasts on resyncMaxS and re-syncs off its next addressed
  // frame.
  const bool authenticated =
      this->hasNetKey() &&
      this->beaconMacIsValid(gb->netkeyid, gb->txround, gb->txslot,
                             gb->pendingmask, gb->pendingmaskvalid,
                             gb->mac.data, gb->mac.len);
  if (this->hasNetKey() && !authenticated)
  {
    ESP_LOGW(TAG, "   GridBeacon: bad MAC or wrong key id (%08x, ours %08x) — "
                  "ignored", (unsigned) gb->netkeyid,
             (unsigned) this->net_key_id_);
    return;
  }

  // One copy, not a burst: a beacon that repeated would occupy the slots after
  // it, which is exactly what the beacon slot's clearance rule exists to stop.
  // burstIndex is read anyway rather than assumed, because assuming it is what
  // put the anchor a non-integral number of slots out when GridSync did it.
  const uint32_t burst_index =
      (outer_header != nullptr) ? outer_header->burstindex : 0u;
  const int64_t t0_measured =
      phase::t0FromRx(rx_us, (uint32_t) this->last_rx_len_)
      - (int64_t) burst_index * drift::kCopySpacingUs;

  const int64_t predicted =
      gridstate::beaconT0ForRound(this->grid_, gb->txround);
  const int64_t err = t0_measured - predicted;

  // MAC-0 clock discipline. The capture for re-anchoring and learning is half a
  // slot pitch, not the guard.
  //
  // REFUTED 2026-09-13: this used to reject anything outside the guard as "not
  // drift — a foreign frame or a node that has already lost the grid". Measured
  // under the production profile the node clock runs +60 ppm against the hub,
  // ~21 ms per 5.8 min beacon interval: outside the guard, and drift. The rule
  // threw away exactly the beacons that could have corrected it. Beyond half a
  // pitch the frame is nearer a neighbouring slot than our mark, and is refused.
  if (!gridstate::beaconErrLearnable(err, this->grid_.params.pitch_us))
  {
    ESP_LOGW(TAG, "   GridBeacon: %lld us off our own beacon mark — beyond half "
                  "a slot pitch, not re-anchoring", (long long) err);
    return;
  }

  // Learn the rate. The previous accepted beacon re-anchored the error to zero,
  // so this error is the residual drift over the hub-clock span since then.
  const int64_t beacon_hub_us =
      gridstate::beaconHubInstantUs(this->grid_.params, gb->txround);
  if (this->have_rate_ref_ && beacon_hub_us > this->rate_ref_hub_us_)
  {
    const int64_t span_us = beacon_hub_us - this->rate_ref_hub_us_;
    const int32_t residual_ppb = gridstate::residualRatePpb(err, span_us);
    if (residual_ppb != 0)
    {
      this->clock_rate_ppb_ = gridstate::clampRatePpb(
          (int64_t) this->clock_rate_ppb_ + (int64_t) residual_ppb);
      this->grid_.rate_ppb  = this->clock_rate_ppb_;
      // A new rate is a new prediction. Samples taken under the old one describe
      // a grid that no longer exists, and one of them outside the guard would
      // latch outside_guard for the rest of the run.
      this->phase_.reset();
      ESP_LOGI(TAG, "   GridBeacon: clock rate %+d ppb (residual %+d ppb over %lld s)",
               (int) this->clock_rate_ppb_, (int) residual_ppb,
               (long long) (span_us / 1000000));
    }
  }

  // Re-solve the anchor so the CURRENT rate predicts exactly the beacon just
  // measured. With an unchanged rate this is anchor += err; after a rate update
  // the prediction of this very round has moved, and += err would be wrong.
  const int64_t beacon_offset_us =
      gridstate::beaconT0ForRound(this->grid_, gb->txround) - this->grid_.anchor_us;
  this->grid_.anchor_us  = t0_measured - beacon_offset_us;
  this->grid_.last_round = gb->txround;
  // Re-anchored on a settled clock: the boot-minute anchor is gone.
  if (nodeclock::settled())
    this->grid_provisional_ = false;
  this->rate_ref_hub_us_ = beacon_hub_us;
  this->have_rate_ref_   = true;
  this->noteAnchorFixed_();

  // The beacon is the frame the node's phase tracking exists to measure: it is
  // the only one guaranteed to arrive on a known mark whether or not this node
  // has traffic. Sampling it is what lets phaseTrustworthy() mean something on
  // an idle node.
  //
  // Only a beacon inside the GUARD is a promotion sample: learning may use the
  // wider capture above, promotion may not. The rest of this handler still runs
  // for a learnable beacon outside the guard — it carries a valid pending mask
  // and proves the node was heard.
  if (gridstate::reanchorIsSane(err, timedgrid::kGuardUs))
  {
    phase::Sample sample;
    sample.t0_measured_us = t0_measured;
    sample.t0_expected_us = predicted;
    phase::commit(this->phase_, sample, timedgrid::kGuardUs);
  }

  // Section 4.4 Tier 3, and the one thing an unauthenticated beacon may NOT do.
  //
  // A clear bit is an instruction to stop listening for up to a beacon interval
  // — 5.8 minutes at the default cadence — so honouring it from an
  // unauthenticated frame would hand anyone in radio range a cheap way to
  // silence the fleet: far cheaper than jamming, and invisible, because a node
  // that skips wrongly reports nothing. The rule is that an unauthenticated
  // frame may make this node listen MORE and never less.
  //
  // The gate used to be frame_authenticated_, which is the AEAD flag — and it
  // is false for every broadcast by construction, so the branch could never be
  // taken and Tier 3's saving was unreachable rather than merely unused. It is
  // now the beacon's OWN authenticator: an AES-CMAC under the fleet key, which
  // is what a one-to-many frame can carry and a per-node session key cannot.
  //
  // Note what this does NOT change: the hub still publishes all-set, because a
  // cleared bit is a promise it cannot keep for an interactive node. Being able
  // to authenticate the mask and being willing to clear a bit are two separate
  // decisions, and only the first is made here.
  this->pending_.beacon_round        = gb->txround;
  this->pending_.beacon_every_rounds = this->grid_.params.beacon_every_rounds;
  if (authenticated)
  {
    this->pending_.bits  = gb->pendingmask;
    this->pending_.valid = gb->pendingmaskvalid;
  }
  else
  {
    this->pending_.bits  = 0;
    this->pending_.valid = false;      // unknown means listen
  }

  // The staleness clock. A beacon IS the hub, heard on a mark this node
  // predicted, so it is exactly the evidence resyncMaxS asks for — and it is
  // the reason a node with no traffic of its own can stay in Mode B at all.
  this->last_addressed_us_ = nodeclock::nowUs();
  this->expected_t0_us_    = gridstate::nextT0Us(this->grid_, rx_us);

  ESP_LOGD(TAG, "   GridBeacon: round %u, re-anchored by %+lld us, mask %08x "
                "valid %d",
           (unsigned) gb->txround, (long long) err,
           (unsigned) gb->pendingmask, (int) gb->pendingmaskvalid);

  // The mask and the anchor just changed; the receive task may already be
  // asleep on a one-shot aimed under the old ones (see requestRearm).
  if (this->loraIf != nullptr)
    this->loraIf->requestRearm();
}

void CmdDispatcher::dispatchCommand(LoraClientOperationMessage *message_to_process,
                                    const LoraHeader *outer_header)
{
  switch (message_to_process->cmd_case)
  {
  case LORA_CLIENT_OPERATION_MESSAGE__CMD__NOT_SET:
    handleNotSet(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION:
    handleOperation(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP:
    handleSysop(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG:
    handleClientConfig(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC:
    handleTimeSync(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_SCHEDULE:
    handleSchedule(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_DRIFTTEST:
    handleDriftTest(message_to_process, outer_header);
    break;
  // Bench-only on-hardware validation mode. Like MacControl and GridSync this
  // is MAC-layer state and terminates here: no application handler is reachable
  // from this branch, which is what makes the frames it produces safe to send
  // in any session state.
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_MODETEST:
    handleModeTest(message_to_process, outer_header, this->drift_rx_us_);
    break;
  // MAC-0. Terminates here: no application handler is reachable from this
  // branch, which is what makes the frame safe to send at any time.
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL:
    handleMacControl(message_to_process, outer_header, this->drift_rx_us_);
    break;
  // B3. Also MAC-layer state: terminates here, reaches no application handler.
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDSYNC:
    handleGridSync(message_to_process, outer_header, this->drift_rx_us_);
    break;
  // Section 4.4's broadcast beacon. Also MAC-layer: it never reaches an
  // application handler, and it addresses the whole fleet rather than a node.
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDDEMOTE:
    handleGridDemote(outer_header);
    break;

  case LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON:
    handleGridBeacon(message_to_process, outer_header, this->drift_rx_us_);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN:
    handleLogin(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_BASENONCE:
    handleBaseNonce(message_to_process, outer_header);
    break;
  case LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG:
    handleCoverConfig(message_to_process, outer_header);
    break;
  default:
    ESP_LOGE(TAG, "   No valid command");
    break;
  }
}

// Decrypt a downlink frame in place of the caller's message.
//
// Returns the decrypted message (caller takes ownership) or nullptr if the
// frame is not for us / not authentic. Every failure here is expected traffic
// on a shared channel, not a fault: LoRa is broadcast, so this node hears the
// hub's frames for the OTHER node and both nodes' uplinks. Hence the debug/warn
// levels rather than errors.
LoraClientOperationMessage *CmdDispatcher::decryptDownlink(
    LoraClientOperationMessage *rcv_message, const LoraHeader *outer_header)
{
  LoraClientOperationMessage *decrypted_message = nullptr;
  ESP_LOGI(TAG, "Encrypted payload received, attempting decryption");

  // Use the header msgid as the unified frame counter.  Replay protection is
  // NOT done here — it is the acceptRxId() window in admitFrame(), which runs
  // after this returns, so a frame is decrypted before it is judged a replay.
  // That order is deliberate: the cross-talk guard in onReceiveNew has already
  // dropped frames for other nodes, and a forged frame cannot survive the GCM
  // tag anyway.  This is a downlink
  // (hub->node) frame, so set the direction bit for IV separation.
  uint64_t frame_counter = static_cast<uint64_t>(outer_header->msgid) | kDownlinkNonceFlag;

  uint32_t base_nonce;
  if (!get_base_nonce(outer_header->senderaddress, base_nonce))
  {
    // P3: no shared key with this sender => it is another node's uplink
    // overheard on the shared channel (cross-talk), not a hub frame for us.
    // Benign; drop and log at debug.
    ESP_LOGD(TAG, "No base nonce for peer %u (foreign uplink), dropping", outer_header->senderaddress);
    return nullptr;
  }

  // Lightweight format: the IV is not transmitted — derive it locally from
  // base_nonce || (uint64) msgid.  A wrong msgid/nonce makes the GCM tag
  // verification fail below, so an explicit IV compare is redundant.
  uint8_t iv[kAesGcmIvBytes];
  if (!derive_gcm_nonce(outer_header->senderaddress, frame_counter, iv))
  {
    ESP_LOGE(TAG, "Failed to derive GCM nonce for peer %u", outer_header->senderaddress);
    return nullptr;
  }

  uint8_t key[kAesGcmKeyBytes];
  if (!derive_aes_gcm_key(key))
  {
    ESP_LOGE(TAG, "Failed to derive AES-GCM key");
    return nullptr;
  }

  uint8_t aad[20];
  size_t aad_len = 0;
  if (!build_header_aad(outer_header, aad, &aad_len))
  {
    ESP_LOGE(TAG, "Failed to build AAD");
    return nullptr;
  }

  size_t cipher_len = rcv_message->encrypted->ciphertext.len;
  uint8_t *plaintext = static_cast<uint8_t *>(malloc(cipher_len));
  if (!plaintext)
  {
    ESP_LOGE(TAG, "Memory allocation failed for plaintext");
    return nullptr;
  }

  if (!decrypt_payload_gcm(key,
                           iv,
                           aad,
                           aad_len,
                           rcv_message->encrypted->ciphertext.data,
                           cipher_len,
                           rcv_message->encrypted->tag.data,
                           rcv_message->encrypted->tag.len,
                           plaintext))
  {
    // M2 stage 7 (MAC-2). Non-zero here is a SESSION bug, never a channel
    // effect: a corrupted frame fails CRC long before it reaches the MIC. The
    // common benign case is a neighbour's downlink under the same hub key,
    // which the address filter above should already have removed.
    this->noteFrameMic(false);
    // P3: tag mismatch.  Most commonly a hub downlink addressed to another node
    // overheard on the shared channel (same hub key, different counter) — seen
    // daily at ~23:00 during node 1's login handshake.  Could also be a
    // corrupted hub frame for us (the hub then retransmits).  Warn, not error.
    ESP_LOGW(TAG, "AES-GCM auth failed (foreign/corrupt frame), dropping");
    free(plaintext);
    return nullptr;
  }

  this->noteFrameMic(true);
  decrypted_message = lora_client_operation_message__unpack(NULL, cipher_len, plaintext);
  free(plaintext);
  if (decrypted_message == NULL)
  {
    ESP_LOGE(TAG, "Could not unpack decrypted protobuf payload");
    return nullptr;
  }

  return decrypted_message;
}

// Should this frame be acted on at all?
//
// Three gates whose ORDER is load-bearing, each learned from a live failure —
// see the comments inside. Returns false if the frame must be dropped.
bool CmdDispatcher::admitFrame(LoraClientOperationMessage *message_to_process,
                               const LoraHeader *outer_header, bool was_encrypted)
{
  // Address check FIRST — skipped ONLY for CMD_CLIENTCONFIG, because a fresh
  // node has cfgAddress=0 and cannot yet match the hub's intended destaddress;
  // the MAC check inside the CLIENTCONFIG handler is the gate there.
  //
  // P1: this MUST run BEFORE the message-ID check below.  rx_message_id_ is a
  // single counter and LoRa is a shared medium, so a frame addressed to ANOTHER
  // node (overheard) would otherwise advance our rx_message_id_ at the msgid
  // check and only then be dropped here — polluting our counter with a peer's
  // sequence and causing the hub's next real command to us to be rejected as a
  // replay (the observed OTA/SLEEP drop after a hub reboot).  Filtering foreign
  // frames first keeps rx_message_id_ tied to OUR hub->node stream only.
  //
  // CMD_LOGIN is NOT exempt: the hub always addresses LoginMsg to a specific
  // node's short_address (never broadcast), so a LOGIN whose destaddress does
  // not match cfgAddress is meant for a different node on the same air.
  // Deferred to below the plaintext gate. A frame the node REFUSES is not
  // evidence that the node is being served: it cannot be acted on. Counting it
  // as a mark hit reset missed_marks_ and last_addressed_us_, and after a hub
  // reboot the hub's plaintext bursts occupy most of every round, so they walk
  // through a node's stale window and reset BOTH essentially every time. A node
  // that was in Mode B when the hub restarted stayed pinned to a dead anchor
  // indefinitely, arming windows at marks that no longer exist, while its own
  // diagnostics reported a healthy link.
  //
  // The gate matters MORE since 2026-09-20, not less. Demotion now rests
  // entirely on last_addressed_us_ (Demotion::SyncStale, the beacon heartbeat),
  // so a refused frame resetting that clock is the only way left for the node
  // to be talked out of noticing its own deafness.
  bool mark_hit_pending = false;

  if (message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG)
  {
    const bool unicast = (outer_header->destaddress == sysCtrl->getConfigAddress());
    const bool mine =
        unicast || (outer_header->destaddress == loraIf->broadcastAddressing);
    // M2 stage 5, BOTH outcomes. Counting only the rejects left `addressed`
    // permanently zero, so the stage 4->5 pass rate read as 100 % loss; and
    // the plaintext path did not count at all, so `foreign` under-counted too.
    this->noteFrameAddressed(mine);
    // WMR's hit half, and the demotion counter's reset. Both key on "addressed
    // to ME", never on "something arrived": a window walked through by another
    // node's burst is not empty, and counting it as a hit would stop a node
    // that is being served nothing from ever demoting.
    //
    // UNICAST ONLY (2026-09-20). This used to accept `mine`, which includes the
    // BROADCAST address — and GridBeacon and GridDemote are broadcast
    // (lora_tracker.cpp, header.destaddress = broadcastAddressing). So every
    // beacon closed whatever grid mark happened to be open, inflating
    // windows_hit by ~1 per 233 rounds and resetting missed_marks_ on a frame
    // that says nothing about whether THIS node's window worked. The phase path
    // has always refused CMD_GRIDBEACON explicitly for the same reason; the hit
    // path never did. A mark is a promise to send something to THIS node, so
    // only a frame addressed to this node can keep it.
    if (unicast)
      mark_hit_pending = true;
    if (!mine)
    {
      ESP_LOGI(TAG, "This message is not for me.");
      return false;
    }

    // The phase sample is COMMITTED further down, past the plaintext gate.
    // See the block guarded by mark_hit_pending.
  }

  // if message is for this device, or broadcast, print details:
  ESP_LOGI(TAG, "Received packet:");
  ESP_LOGI(TAG, "   From: 0x%x", (unsigned int)outer_header->senderaddress);
  ESP_LOGI(TAG, "   To: 0x%x", (unsigned int)outer_header->destaddress);
  ESP_LOGI(TAG, "   Message ID: %d", (unsigned int)outer_header->msgid);
  ESP_LOGI(TAG, "   RSSI: %d", lora_packetRssi());

  // Ordered BEFORE the message-ID check below, and that ordering is the point.
  // The replay window is a counter ratchet: any frame that passes it advances
  // rx_message_id_. Running it on frames that have not been authenticated let
  // one injected plaintext frame at msgid = observed + 1024 push the counter
  // past every command the hub had queued, so the node rejected the hub's real
  // traffic as replays until the next LOGIN. Rejecting unauthenticated frames
  // first means a forged frame cannot touch the counter at all.
  //
  // Part B: once a session (base nonce) exists, command-plane messages MUST
  // arrive encrypted.  Reject forged/replayed PLAINTEXT commands
  // (OPEN/CLOSE/OTA/SLEEP) an attacker could otherwise inject on-air.  Bootstrap
  // messages (LOGIN / BASENONCE / CLIENTCONFIG) stay plaintext by design and are
  // not gated here.  Before login (no base nonce) plaintext is still accepted so
  // the very first provisioning/command path is unaffected.
  // Once a session exists, EVERY command must be encrypted — not just the two
  // that used to be named here.
  //
  // This check listed CMD_OPERATION and CMD_SYSOP only, so seven other
  // authority-bearing downlinks were accepted in plaintext by a node with a
  // live session: ScheduleConfig, TimeSync, CoverConfig, ClientConfig,
  // GridSync, ModeTest, DriftTest and BaseNonceExchange. The LoraHeader is
  // plaintext on every frame, so an attacker in radio range reads destAddress
  // and msgid directly and needs only msgid in (rx_id_, rx_id_ + 1024]. From
  // one unauthenticated ~30-byte frame:
  //
  //   ScheduleConfig     replaces the schedule WHOLESALE — it is idempotent by
  //                      design — so "SCHED_OPEN 03:00, every day" opens every
  //                      blind every night. A physical-security bypass with no
  //                      key.
  //   BaseNonceExchange  installs an attacker-chosen nonce and persists it to
  //                      NVS immediately. The node can no longer decrypt the
  //                      hub and its uplinks fail the hub's tag check. Survives
  //                      deep sleep. Repeat once a second for a fleet DoS.
  //   ClientConfig       re-addresses the node or sets its sleep duration; it
  //                      also skips the address filter, so its only gate is a
  //                      MAC match — and the MAC is broadcast in the plaintext
  //                      ClientRegister.
  //   TimeSync           settimeofday from an unauthenticated frame.
  //
  // CMD_LOGIN is the one exemption, and it is exactly one because LoginMsg
  // CARRIES the base nonce (see handleLogin) — it is the whole bootstrap. The
  // hub sets session_confirmed_ = false before sending it precisely so it goes
  // out in the clear, which is what lets a node whose nonce has diverged
  // recover.
  //
  // BaseNonceExchange gets no exemption, and that is a constraint on the HUB,
  // not an observation about it: the hub's "no base nonce for this peer"
  // recovery used to send a PLAINTEXT CMD_BASENONCE, which this gate refuses —
  // correctly, since an unauthenticated frame offering a node a new key is
  // exactly what a node holding a session must not act on. The hub now
  // re-provisions through send_login() instead, which carries the same nonce
  // under the exemption above. Likewise ClientConfig: it sets this node's
  // address, subnet, name and sleep duration, so the hub defers it until the
  // session is confirmed and sends it encrypted. If either path regresses to
  // plaintext, this gate is what stops it, and the link goes quiet rather than
  // insecure.
  //
  // The test is "does THIS NODE hold a session", asked of the node's own state
  // and of nothing on the wire.
  //
  // It used to ask "do I hold a base nonce for the SENDER" — and senderaddress
  // is a plaintext field the node never validates. Naming a sender the node had
  // never heard of made get_base_nonce fail, and the gate did not fire at all:
  // the frame went on to ratchet rx_message_id_, set destAddress to the
  // attacker's address, and reach ScheduleConfig, BaseNonceExchange, GridSync,
  // TimeSync and CoverConfig. Worse, findOrCreatePeer evicts slot 0 when the
  // four-entry peer table is full, so four frames naming four unknown senders
  // evict the hub's real base nonce. The gate was keyed on the one thing the
  // attacker chooses.
  //
  // A node that has never been provisioned holds no session, so bootstrap is
  // unaffected and plaintext still reaches it — which is the whole point of
  // the distinction.
  // CMD_GRIDBEACON is exempt, and it is the one exemption added since this gate
  // was written, so the reasoning is here rather than in the handler:
  //
  //   * It CANNOT be encrypted. One broadcast serves 32 nodes and the sessions
  //     are per-node, so there is no key it could be SEALED with. It is now
  //     SIGNED instead, which is the part a one-to-many key can do: an AES-CMAC
  //     under the fleet key, checked in handleGridBeacon. Exempting it from
  //     this gate is therefore still right — the gate asks "was this
  //     encrypted?", and the beacon's answer is permanently no.
  //   * Its effects are bounded by the node's own arithmetic as well as by the
  //     MAC. The anchor moves only by what fits inside the guard band
  //     (gridstate::reanchorIsSane) even for a beacon that verifies, because a
  //     MAC proves the sender held the fleet key and every node holds it too.
  //   * Replay is answered by the same arithmetic, which is why the MAC carries
  //     no timestamp. A replayed beacon declares the round it was minted for,
  //     so its predicted mark is rounds in the past and the error is orders of
  //     magnitude outside the guard.
  //
  // What that leaves — and it is the gap the MAC was added to close: an
  // attacker transmitting real beacons at the right instants could nudge the
  // anchor by up to a guard band EACH TIME, and the guard bounds one nudge
  // rather than a sequence. A node that holds a fleet key now refuses an
  // unsigned beacon outright, so the walk is closed for a keyed node. A node
  // that holds none still has it, which is the pre-existing behaviour and is
  // recorded in 11b.
  //
  // CMD_GRIDDEMOTE is the second exemption, and it is the cleanest of the
  // three. It CANNOT be encrypted for the same reason the beacon cannot — one
  // broadcast, 32 per-node sessions — and it is the frame the hub sends when it
  // has just restarted and holds no session at all. Its only effect is to throw
  // an anchor away, so it satisfies "may make this node listen MORE and never
  // less" outright: Mode A is three windows per round against Mode B's one, and
  // it is the mode the fleet ships in. It carries no fields, so there is
  // nothing for a sender to choose; the handler additionally requires a
  // BROADCAST and refuses to clear the fleet key, which is what stops it being
  // a way to re-open the anchor walk the key closed.
  // A MAC PING while an authenticated ModeTest is running is the fourth
  // exemption, and the narrowest. The frames a test MEASURES are deliberately
  // unauthenticated (mac-layer.md section 4: only ARMING must be); before this
  // the gate refused every hub ping on a node that held a session, so the
  // turnaround histogram (HW-7, MAC-1's cost in time) had `n 0` in every run
  // ever taken. Exactly a PING, and only while `mode_test_active_` - which an
  // encrypted, authenticated ModeTest arms and a node-owned deadline ends - so
  // outside a test a plaintext frame still cannot make this node transmit.
  // MAC_CONFIG, the frame that changes what the node enforces, stays refused.
  //
  // Also gated on MAC-2: `cryptoRequired(sublayers_, is_mac_control=true)` is
  // `sublayers_.crypto_enabled`, and until now nothing anywhere called it — the
  // switch had zero effect on this gate, so MAC-2's cost could never be
  // attributed via the turnaround measurement, because the hub's ping was never
  // actually encrypted either way (see build_mac_ping_frame_'s comment on the
  // hub). With MAC-2 on, this exemption no longer covers a PLAINTEXT ping: only
  // an ENCRYPTED one reaches MAC-0, the same way every other downlink does
  // (`was_encrypted` true, which already bypasses this whole gate below).
  const bool ping_during_test =
      this->mode_test_active_ &&
      !macsublayers::cryptoRequired(this->sublayers_, /*is_mac_control=*/true) &&
      message_to_process->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL &&
      message_to_process->maccontrol != nullptr &&
      message_to_process->maccontrol->kind == MAC_CONTROL__KIND__MAC_PING;

  if (!was_encrypted && !ping_during_test &&
      message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN &&
      message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON &&
      message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDDEMOTE)
  {
    // AN UNPROVISIONED NODE'S BOOTSTRAP. Its config (LittleFS) and its session
    // (NVS) are stored apart, and the config can be lost alone. Measured
    // 2026-09-15 on node 2 (fw 1.0.85): a reset during a config save left
    // config.txt empty — address 0 — while NVS still restored the session. The
    // node sent REGISTER with needs_config, the hub pushed ClientConfig in the
    // clear as that asks, and the node refused it here, every minute, for good.
    // A session belongs to a provisioning; with no address there is none to
    // protect, so the frames that provision a node pass. Nothing else does.
    const bool unprovisioned_bootstrap =
        this->sysCtrl != nullptr && this->sysCtrl->getConfigAddress() == 0 &&
        (message_to_process->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG ||
         message_to_process->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG);
    if ((this->session_.hasValidState() || this->session_proven_) && !unprovisioned_bootstrap)
    {
      ESP_LOGW(TAG, "Rejecting PLAINTEXT command (cmd_case=%d) claiming peer %u — this node holds a session",
               static_cast<int>(message_to_process->cmd_case), (unsigned)outer_header->senderaddress);
      return false;
    }
  }

  // Only now, past every gate: this frame is one the node will actually act on.
  if (mark_hit_pending)
  {
    // The HIT is window-scoped: only the window that armed the mark may keep it.
    this->noteMarkHit(this->last_rx_window_gen_);
    // The two RESETS below are deliberately NOT window-scoped, and that is not
    // an oversight. They answer a different question from "did my window work":
    //   missed_marks_      — is the hub serving this node AT ALL? A unicast
    //                        frame proves it is, whichever window heard it, so
    //                        demoting on top of that would be wrong.
    //   last_addressed_us_ — feeds Demotion::SyncStale, which bounds how long
    //                        the node may hold an anchor with nothing arriving
    //                        for it. Again: arrival is the evidence, not aim.
    // Both are reached only when mark_hit_pending is set, which is now UNICAST
    // only — so a broadcast beacon no longer resets either one.
    this->missed_marks_ = 0;
    this->last_addressed_us_ = nodeclock::nowUs();
  }

  // B2: COMMIT the phase sample here and nowhere else.
  //
  // The arrival instant was captured early, before parsing, which is right for
  // drift — a drift measurement only cares when a frame arrived. It is wrong
  // for phase: on a shared channel most frames a node hears are aimed at a slot
  // 46.875 ms from its own, so stamping them makes phaseErrUs bimodal (a
  // cluster at 0, a cluster at one pitch) and its mean describes nothing.
  // Capture early, commit late.
  //
  // PAST THE PLAINTEXT GATE, which is where it moved (11b). It used to sit up
  // with the address filter, so anything in radio range could feed the fit that
  // decides whether this node trusts its own anchor. Bounded — poisoning it
  // drives outside_guard up and DEMOTES the node rather than desynchronising it
  // silently — but a bound is not a reason to accept the input. On a node with
  // no session nothing changes: the gate does not fire there, which is the same
  // bootstrap carve-out ClientConfig has.
  //
  // Only with a grid: an expected T0 of 0 would turn every error into the
  // node's whole uptime.
  //
  // ONLY A FRAME THE HUB PLACED ON THIS NODE'S MARK (header onMark). Measured
  // 2026-09-14 on node 2: two 900 s Mode B production runs armed no window
  // (windows 0/0) while every ModeTest mark read inside the guard (p99
  // 9 377 us). The ModeTest START is unplaced; it arrived 463 ms off the mark,
  // was committed here as a phase error, latched outside_guard, and
  // phaseTrustworthy() — which requires zero samples outside — kept the node
  // on NoPhase for the whole run. Any unplaced downlink did the same in
  // production: a Mode A burst, a TimeSync, a Class A reply aimed at RX1. Their
  // arrival says where the hub happened to send, not where the node's mark is.
  //
  // OR ANY COPY THAT DECLARES ITS FIRE INSTANT (fireStamped), placed or not. The
  // declaration is the expectation, so a LOGIN burst is as good a sample as a
  // mark, and a late frame measures nothing false. This is what brings the eight
  // samples promotion needs from ~23 s per lucky mark catch down to the first
  // bursts after a login.
  if (mark_hit_pending && (outer_header->firestamped || outer_header->onmark) &&
      this->grid_.active && this->drift_rx_us_ != 0 &&
      message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON)
  {
    // BACK OUT THE BURST COPY, as handleGridSync and handleGridBeacon already
    // do. The declaration and the marks describe COPY 0; copies are one 88 ms
    // stride apart. Stamping copy N as if it were copy 0 commits an error of
    // N x 88 ms against a 14 080 us guard, and phaseTrustworthy() requires ZERO
    // samples outside the guard — so a single heard retransmission made the
    // node distrust its own anchor permanently. It is not a rare case either:
    // until the hub promotes to single-shot it sends seventeen copies, and a
    // node that has not promoted yet is sweeping a free-running window, so the
    // copy it hears first is usually not copy 0.
    // A stamped copy declares its OWN instant, so there is nothing to back out.
    const bool stamped = outer_header->firestamped;
    const uint32_t burst_index = stamped ? 0u : outer_header->burstindex;
    const int64_t measured =
        phase::t0FromRx(this->drift_rx_us_, (uint32_t) this->last_rx_len_)
        - (int64_t) burst_index * drift::kCopySpacingUs;

    // A PROVISIONAL grid — adopted before node time settled — takes no samples:
    // measured 2026-09-14, an anchor from 10-14 s after boot left every later
    // mark 8 ms off. Once the clock has settled, the first stamped frame
    // re-solves the anchor from the instant it declares, exactly as a stamped
    // GridSync would, and sampling starts from there.
    bool sample_it = true;
    if (this->grid_provisional_)
    {
      sample_it = false;
      if (stamped && nodeclock::settled())
      {
        const int64_t before = this->grid_.anchor_us;
        this->grid_.anchor_us = gridstate::solveAnchorFromHubInstantUs(
            measured, outer_header->fireround, outer_header->fireoffsetus,
            this->grid_.params, this->clock_rate_ppb_);
        this->grid_provisional_ = false;
        // Exact at this frame's declared instant: the rate span starts here.
        this->have_rate_ref_   = true;   // re-solve starts the span
        this->rate_ref_hub_us_ = gridstate::hubInstantUs(
            this->grid_.params, outer_header->fireround, outer_header->fireoffsetus);
        this->phase_.reset();
        this->noteAnchorFixed_();
        ESP_LOGW(TAG, "   grid anchor re-solved on a settled clock: moved %+lld us",
                 (long long) (this->grid_.anchor_us - before));
        if (this->loraIf != nullptr)
          this->loraIf->requestRearm();
      }
    }

    // Predict the mark this frame belongs to, from the grid, EVERY TIME.
    //
    // expected_t0_us_ used to be assigned once at grid adoption and never
    // advanced (setExpectedT0Us had no caller anywhere). Every sample after the
    // first was therefore measured against a mark one round further in the
    // past: sample 2 read +1.5 s, sample 3 +3.0 s, and so on.
    // phaseTrustworthy() requires zero samples outside the guard, so it went
    // permanently false on the SECOND addressed frame and timedRxActive()
    // reported NoPhase forever. Mode B could never be entered, and the beacon's
    // phaseErrUs — gated at "±2 ms in the field" — reported multi-second
    // values. The failure was silent and looked like a clock fault.
    //
    // The nearest mark, not the next one: a frame that arrives a hair EARLY
    // belongs to the mark ahead of it, and nextT0Us would charge it a whole
    // round of error.
    const int64_t next_t0 = gridstate::nextT0Us(this->grid_, measured);
    const int64_t prev_t0 = next_t0 - (int64_t) this->grid_.params.round_us;
    const int64_t nearest_mark =
        ((next_t0 - measured) <= (measured - prev_t0)) ? next_t0 : prev_t0;
    const int64_t expected =
        stamped ? gridstate::t0ForHubInstantUs(this->grid_, outer_header->fireround,
                                               outer_header->fireoffsetus)
                : nearest_mark;

    this->expected_t0_us_ = nearest_mark;   // reported in the beacon: a MARK
    const phase::Sample sample{measured, expected};
    // Copies of one frame share its timing; see phase::Stats::frames.
    const bool new_frame = (outer_header->msgid != this->last_phase_msgid_);
    this->last_phase_msgid_ = outer_header->msgid;
    // Further than half a slot pitch from its own instant is not drift and not
    // this frame's mark — the rule beacons already use. Measured 2026-09-15 on
    // node 2 (fw 1.0.88): one sample right after the anchor re-solve read
    // +86 841 us, latched outside_guard, and held promotion off for 830 s until a
    // rate update reset the statistics. Refused, not committed.
    if (sample_it &&
        !gridstate::beaconErrLearnable(measured - expected, this->grid_.params.pitch_us))
    {
      ESP_LOGW(TAG, "   phase sample refused: err %lld us (len %u, msgid %u) is beyond half "
                    "a slot pitch", (long long) (measured - expected),
               (unsigned) this->last_rx_len_, (unsigned) outer_header->msgid);
      sample_it = false;
      if (this->refused_since_fix_ < 0xFFFFFFFFu)
        this->refused_since_fix_++;
    }
    if (sample_it)
    {
      phase::commit(this->phase_, sample, timedgrid::kGuardUs, new_frame);
      // One line per sample, with the frame LENGTH. It is what found the boot-
      // minute anchor (2026-09-14): marks read -8 ms against a GridSync adopted
      // 10 s after boot, and ~0 against the same GridSync adopted at 297 s.
      // Few frames per minute reach here.
      ESP_LOGI(TAG, "   phase sample: len %u stamped %d err %d us (n %u frames %u outside %u)",
               (unsigned) this->last_rx_len_, (int) stamped, (int) this->phase_.last_us,
               (unsigned) this->phase_.n, (unsigned) this->phase_.frames,
               (unsigned) this->phase_.outside_guard);

      // A kept grid, re-centred on the trial's own evidence
      // (timedmode::trialRecenterUs): samples that agree off-centre describe a
      // shifted anchor. Measured 2026-09-15 on node 2 (fw 1.0.92): after ~17
      // minutes in Mode A the node re-promoted 8-11 ms off its marks.
      const int32_t shift = timedmode::trialRecenterUs(
          this->last_reason_logged_ == (uint8_t) timedmode::Demotion::None,
          this->grid_provisional_, this->phase_.n, this->phase_.frames,
          this->phase_.mean_us(), this->phase_.spread_us(),
          timedgrid::kGuardUs, this->grid_.params.pitch_us);
      if (shift != 0)
      {
        const uint32_t on_samples = this->phase_.n;
        this->grid_.anchor_us += shift;
        // The shift is not drift over the rate span the next beacon divides by;
        // counting it there would teach the node a false rate. Start a new span.
        this->have_rate_ref_ = false;
        // The samples describe the anchor that was just moved.
        this->phase_.reset();
        this->noteAnchorFixed_();
        ESP_LOGW(TAG, "   grid anchor re-centred on %u samples: moved %+d us",
                 (unsigned) on_samples, (int) shift);
        if (this->loraIf != nullptr)
          this->loraIf->requestRearm();
      }
    }
  }

  // A node holding a grid it can no longer trust asks for it again
  // (maybeRequestGridSync). Per addressed frame: no timer, no extra wake.
  this->maybeRequestGridSync();

  // -------------------------------------------------------------------------
  // Message ID check — applied to all messages except CMD_LOGIN.
  // CMD_LOGIN resets rx_message_id_ to 0 unconditionally; subjecting it to
  // the msgid check would cause a fresh LoginMsg (msgid=1) to be silently
  // dropped as a replay whenever rx_message_id_ > 1.  CMD_LOGIN is protected
  // against burst duplicates by its own 5 s rate limiter instead.
  // Runs AFTER the address filter above so only frames addressed to us can
  // advance rx_message_id_ (see P1 note above).
  // -------------------------------------------------------------------------
  //
  // CMD_CLIENTCONFIG is exempt for a second reason, and this is a hole in the
  // P1 fix above rather than a new rule: CLIENTCONFIG is the ONE message that
  // skips the address filter, so a CLIENTCONFIG addressed to a DIFFERENT node
  // reaches this check and, if its msgid happens to be higher than ours, would
  // ratchet rx_message_id_ onto that node's sequence — the exact counter
  // pollution the address filter was moved up to prevent.
  //
  // Observed live on node 2 (address 18): a ClientConfig for node 1 reached
  // this check and was evaluated against node 2's counter —
  //     Dest Adreess: 17 / Config Address: 18
  //     Message ID check
  //     Rejected message ID: 2, ignoring, my MsgID: 3
  // It was harmless only because 2 < 3. With the ordering reversed it would
  // have wedged node 2's link until its next login.
  //
  // The gate for CLIENTCONFIG is the MAC check inside its handler, which is
  // strictly stronger than an address match, so dropping the msgid check here
  // loses nothing: a foreign CLIENTCONFIG is still rejected, just without
  // touching our counter first.
  //
  // CMD_GRIDBEACON is exempt for a third reason: it is a BROADCAST, so it
  // belongs to no per-node sequence at all. Running it through this check would
  // ratchet rx_message_id_ onto the beacon's own counter and wedge the node's
  // link to the hub until its next login — the same counter pollution the
  // address filter was moved up to prevent, arriving by a different door. Its
  // replay protection is its timing: see the plaintext gate above.
  // CMD_GRIDDEMOTE is exempt for exactly the beacon's reason: it is a broadcast,
  // so it belongs to no per-node sequence, and running it through this check
  // would ratchet every node's rx counter onto the hub's broadcast numbering
  // and wedge the link until the next login. A replayed demote simply demotes
  // an already-demoted node, which is idempotent and in the safe direction, so
  // there is nothing for the counter to protect here.
  if (message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN &&
      message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG &&
      message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON &&
      message_to_process->cmd_case != LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDDEMOTE)
  {
    ESP_LOGI(TAG, "   Message ID check");
    // Accept only a forward jump within a bounded window.  msgid increments by 1
    // per message (plus a small reboot margin), so a huge jump is a corrupt or
    // spurious frame — ratcheting rx_message_id_ up to it would wedge the link
    // (dropping every legitimate lower id) until the next login resets it.
    // M3: MAC-1 is skippable for MAC CONTROL frames only. Application traffic
    // is never exempt — see MacSublayers.h. Without this a measurement run
    // cannot sweep msgids freely, because the accept window rejects an id far
    // ahead of the node's own counter.
    const bool is_mac_control =
        (message_to_process->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL);

    // Skip the CHECK, not the rest of the function. An early return here would
    // also skip the destAddress/destSubnet refresh below, so an echo would be
    // addressed to whatever peer was seen last — the hub would record 100 %
    // ping loss from a node that is answering perfectly.
    // MAC-1 for a MAC CONTROL frame is not the session's replay window. A ping's
    // msgid is a separate counter the session never saw, so the window would
    // refuse every ping once the counter is on; and ratcheting the SESSION's
    // window from an unauthenticated frame would let anyone in range wedge the
    // link. A ping is sequenced by its own `seq`: strictly increasing, gaps
    // allowed (a lost frame is a hole, not a violation). Only PINGs carry one;
    // other control frames are authenticated and are not sequenced here.
    const bool is_ping =
        is_mac_control && message_to_process->maccontrol != nullptr &&
        message_to_process->maccontrol->kind == MAC_CONTROL__KIND__MAC_PING;
    if (is_ping && this->sublayers_.counter_enabled)
    {
      const uint32_t seq = message_to_process->maccontrol->seq;
      const bool ping_ok = seq > this->mac_ping_seq_hw_;
      if (ping_ok)
        this->mac_ping_seq_hw_ = seq;
      this->noteFrameCounter(ping_ok);
      if (!ping_ok)
      {
        ESP_LOGW(TAG, "   MAC ping seq %u is not above %u - duplicate or replay, ignoring",
                 (unsigned) seq, (unsigned) this->mac_ping_seq_hw_);
        return false;
      }
    }
    else if (!is_mac_control && macsublayers::counterCheckRequired(this->sublayers_, false))
    {
    const bool accepted = this->session_.acceptRxId(outer_header->msgid);
    // M2 stage 6 (MAC-1). In Mode A a rejection here is usually one of the
    // sixteen intentional duplicates in a 17-copy burst, so a high DUP rate is
    // healthy rather than alarming — which is why it is reported as its own
    // rate and not folded into frame loss.
    this->noteFrameCounter(accepted);
    if (!accepted)
    {
      // B4: the frame is a duplicate. That is not the same as "unwanted".
      //
      // Before this, every duplicate was dropped in silence, and the hub's
      // retransmit is byte-identical (pack-once) — same msgid — so a lost ack
      // was UNRECOVERABLE: the hub retried, the node dropped it as a replay,
      // no ack could ever be regenerated, and after kOpMaxRetries the hub tore
      // the session down. Shipping pack-once without this made the retry path
      // strictly worse than the fresh-msgid behaviour it replaced.
      //
      // AckCache.h decides which kind of duplicate this is; the discriminator
      // is time. Sixteen of the seventeen copies of a Mode A burst are
      // duplicates and must stay silent — answering them all would be sixteen
      // uplinks per command on a battery node, worse than the bug.
      const int64_t now_us = nodeclock::nowUs();
      const ackcache::Decision d =
          ackcache::classify(this->ack_cache_, outer_header->msgid, now_us);

      // Encrypted frames only. A duplicate that arrived in plaintext while a
      // session exists must not be able to make the node transmit: that would
      // turn a lost-ack recovery into an unauthenticated uplink trigger. The
      // decryption above is what authenticates the frame, so `was_encrypted`
      // here means the AEAD tag verified.
      if (d == ackcache::Decision::ReAck && was_encrypted)
      {
        ESP_LOGW(TAG, "   Duplicate msgid %u after %lld ms — the ack was lost, "
                      "re-acking (%u of %u)",
                 (unsigned) outer_header->msgid,
                 (long long) ((now_us - this->ack_cache_.firstSeenUs()) / 1000),
                 (unsigned) (this->ack_cache_.reacks() + 1),
                 (unsigned) ackcache::kMaxReAcks);
        ackcache::noteReAck(this->ack_cache_, outer_header->msgid, now_us);
        this->sendCommandAck(outer_header->msgid);
      }
      else
      {
        ESP_LOGE(TAG, "Rejected message ID: %d, ignoring, my MsgID: %d",
                 outer_header->msgid, this->session_.rxId());
      }
      return false;
    }
    }
  }

  // Set LAST, after every gate. This is where our uplinks are aimed, so a frame
  // that was going to be refused must not be able to retarget them — the
  // plaintext gate above now runs first for that reason too.
  this->destAddress = outer_header->senderaddress;
  this->destSubnet  = outer_header->destsubnet;

  return true;
}


// Collect burst copies and fit node-vs-hub clock drift.
//
// A burst is a free ruler: the hub emits copy i at nominal i * 88.235 ms with
// no CAD and no backoff, paced by vTaskDelayUntil(), and every copy carries its
// own burstIndex. Regressing the ISR arrival times against that nominal line
// gives the relative rate of the two clocks — no absolute time, no protocol
// change. See DriftEstimator.h.
//
// Opportunistic by design: with windowed RX the node hears about one copy per
// burst, so a fit almost never has enough points and this quietly does nothing.
// DRIFT_TEST_MODE switches the radio to continuous RX, at which point the node
// hears most of the 17 copies and every burst yields a fit.
void CmdDispatcher::noteBurstArrival_(const LoraHeader *h, int64_t rx_us)
{
  if (h == nullptr || rx_us == 0)
    return;   // 0 means the caller had no ISR timestamp; do not treat it as t=0

  // Identify the burst by the instant its copy 0 would have left the hub. Two
  // copies of the SAME burst agree on this to within the drift being measured;
  // copies of different bursts do not, so it separates them without needing a
  // burst id on the wire.
  const int64_t copy0 = rx_us - (int64_t) h->burstindex * drift::kCopySpacingUs;

  // Group by PROXIMITY to the running reference, not by bucketing copy0 into
  // fixed 1.5 s slots. Bucketing splits a burst in two whenever its copy-0
  // instant happens to sit near a bucket boundary -- observed as one burst
  // arriving as a 7-copy fit followed by two useless 3-copy fragments.
  // Bursts are 15 s apart, so a tolerance of 100 ms separates them with room
  // to spare while absorbing any plausible timestamp noise.
  static constexpr int64_t kSameBurstTolUs = 100000;
  const bool new_burst = (this->drift_n_ == 0) ||
                         (llabs(copy0 - this->drift_copy0_ref_) > kSameBurstTolUs);

  if (new_burst)
  {
    // A new burst. Fit what the previous one gathered before discarding it.
    if (this->drift_n_ >= 3)
    {
      const drift::Result r = drift::estimate(this->drift_samples_, this->drift_n_);
      if (drift::usable(r))
      {
        this->drift_acc_.add(r);
        ESP_LOGW(TAG, "DRIFT: %+d ppm from %u copies, spacing %d us (nominal %d)  (mean %+d ppm over %u bursts)",
                 (int) r.ppm, (unsigned) r.samples,
                 (int) r.measured_spacing_us, (int) drift::kCopySpacingUs,
                 (int) this->drift_acc_.mean_ppm(), (unsigned) this->drift_acc_.count);
      }
    }
    this->drift_copy0_ref_ = copy0;
    this->drift_n_ = 0;
  }

  if (this->drift_n_ < drift::kMaxSamples)
  {
    this->drift_samples_[this->drift_n_].rx_us       = rx_us;
    this->drift_samples_[this->drift_n_].burst_index = h->burstindex;
    this->drift_n_++;
  }
}

void CmdDispatcher::onReceiveNew(uint8_t *rxBuf, int packetSize, int64_t rx_us)
{
  // Stash the ISR-captured arrival instant for this frame so handlers can use
  // it. Set before any dispatch so a DriftTest frame sees its own timestamp.
  this->drift_rx_us_ = rx_us;

  // Cleared HERE, on the dispatcher task, for every frame this function
  // dispatches — not in noteDriftSample(), which runs on the DIO0 task.
  //
  // Those are different tasks separated by a queue and a per-frame delay, so
  // clearing there left the flag STICKY: an encrypted frame set it true, and
  // any plaintext frame already queued behind it was then dispatched with
  // frame_authenticated_ still true. A plaintext MAC_CONFIG could disable the
  // replay window and authentication for 900 s — the exact property
  // MacSublayers.h claims is impossible.
  this->frame_authenticated_ = false;
  this->last_rx_len_ = packetSize;

  LoraClientOperationMessage *rcv_message = lora_client_operation_message__unpack(NULL, packetSize, rxBuf);
  // M2 stage 4. A parse failure is usually a neighbour's frame or noise, not a
  // fault — counted, not logged as an error.
  this->noteFrameParsed(rcv_message != NULL);
  if (rcv_message == NULL)
  {
    // P3: usually a corrupted/foreign frame overheard on the shared channel (LoRa
    // is broadcast; node 2 hears node 1's traffic).  Dropped safely — log at debug
    // so it doesn't read as a fault.
    ESP_LOGD(TAG, "Could not read protobuf (foreign/corrupt frame), dropping");
    return;
  }

  // Copy the outer-header scalar fields into a local struct so they remain
  // valid for the entire function.  In the encrypted path rcv_message is freed
  // (lora_client_operation_message__free_unpacked) before the switch/logging
  // block below, so keeping a raw pointer into its heap allocation would be
  // undefined behaviour.  Only uint32_t / protobuf_c_boolean fields are used
  // after that point, so a shallow copy is sufficient.
  LoraHeader saved_header = LORA_HEADER__INIT;
  if (rcv_message->header)
  {
    saved_header.destaddress   = rcv_message->header->destaddress;
    saved_header.destsubnet    = rcv_message->header->destsubnet;
    saved_header.senderaddress = rcv_message->header->senderaddress;
    saved_header.msgid         = rcv_message->header->msgid;
    // Burst position travels too. It was left out, so every handler reached
    // through dispatchCommand saw burstIndex 0 no matter which copy arrived —
    // silently, because 0 is a legitimate value. handleGridSync needs it to
    // back out this copy's offset before solving the anchor, and anything else
    // downstream that reasons about burst position would have been wrong the
    // same way. Both are uint32_t, so the shallow copy stays sufficient.
    saved_header.burstindex    = rcv_message->header->burstindex;
    saved_header.burstcount    = rcv_message->header->burstcount;
    // And whether it was placed on this node's mark — the phase commit's gate.
    saved_header.onmark        = rcv_message->header->onmark;
    // And where this copy's T0 really is on the hub grid, when the hub said so.
    saved_header.firestamped   = rcv_message->header->firestamped;
    saved_header.fireround     = rcv_message->header->fireround;
    saved_header.fireoffsetus  = rcv_message->header->fireoffsetus;
  }
  LoraHeader *outer_header = &saved_header;
  // Encryption is now inferred from the oneof case (no header flag).  Captured
  // here for the Part B plaintext-command rejection further down.
  const bool was_encrypted =
      (rcv_message->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED);

  // Burst scheduling: if this copy is part of a hub TX burst, estimate when the
  // burst finishes and record it so loraRxTask defers the node's reply until the
  // channel is clear.  Done for EVERY received copy (even msgid-duplicate ones,
  // which are dropped just below) so a later-index copy refines the estimate.
  if (rcv_message->header && rcv_message->header->burstcount > 0)
  {
    this->noteBurstArrival_(rcv_message->header, rx_us);
    uint32_t idx = rcv_message->header->burstindex;
    uint32_t cnt = rcv_message->header->burstcount;
    uint32_t remaining = (idx + 1 < cnt) ? (cnt - 1 - idx) : 0u;

    // Anchor on THIS copy's T0, and include the last copy's air time.
    //
    // This used to read `esp_timer_get_time() + remaining * 88 ms`, which is
    // wrong twice over. `now` is somewhere after RxDone, not at the copy's
    // start, so the whole prediction was already shifted by one frame's air
    // time plus however long the parse took; and it stopped at the moment the
    // last copy BEGINS rather than when it ends. For a 152 B frame that is
    // 95.3 ms of channel the node believed was clear — so it fired its reply
    // into the tail of the burst it was waiting out.
    //
    // T0 is the single timing reference everywhere else in this firmware
    // (see LoraTiming.h); this is no exception, and it is recovered the one
    // sanctioned way rather than open-coded.
    // rx_us is 0 when the caller had no hardware timestamp to give (the
    // wake-source path can reach here without one). Falling back to "now"
    // reproduces the old anchor for that case, which is late by one frame's
    // air time — still better than an anchor computed from zero, which would
    // put the burst's end in the past and defer nothing.
    const uint32_t len    = (uint32_t) this->last_rx_len_;
    const int64_t  rxdone = (rx_us > 0) ? rx_us : nodeclock::nowUs();
    const int64_t  t0_this = phase::t0FromRx(rxdone, len);
    const int64_t  t0_last =
        t0_this + static_cast<int64_t>(remaining) * kBurstTxIntervalMs * 1000;
    this->burst_end_us_ = t0_last + (int64_t) loratiming::t0ToRxDoneUs(len);
  }

  // (b) Cross-talk guard BEFORE decryption.  On the shared LoRa channel we
  // overhear frames addressed to the OTHER node.  The outer header is plaintext,
  // so validate the destination address up-front and drop a foreign encrypted
  // frame before spending any CPU on GCM decryption (and before it can emit
  // decrypt-path log noise).  Encrypted command frames are always unicast-
  // addressed by the hub, so no CLIENTCONFIG exemption is needed here —
  // CLIENTCONFIG is a plaintext bootstrap message handled on the path below.
  if (was_encrypted &&
      outer_header->destaddress != sysCtrl->getConfigAddress() &&
      outer_header->destaddress != loraIf->broadcastAddressing)
  {
    // M2 stage 5: not loss. On a shared broadcast channel every node hears its
    // neighbours, and discarding their traffic is the filter working.
    this->noteFrameAddressed(false);
    // (the matching noteAddressed(true) is in admitFrame, where the plaintext
    // path also decides; counting it only here would leave `addressed`
    // permanently zero and make stage 4->5 read as 100 % loss)
    ESP_LOGD(TAG, "Encrypted frame dest %u not for me — dropping before decrypt",
             (unsigned)outer_header->destaddress);
    lora_client_operation_message__free_unpacked(rcv_message, NULL);
    return;
  }

  // Ownership of the unpacked message(s) for the rest of the function.
  //
  // Before this, every early exit had to free by hand, and because the
  // encrypted path swaps one allocation for another, each site repeated
  // `if (decrypted_message) free(a) else free(b)` — eight exits, sixteen
  // branches, and a leak one edit away at any of them.
  struct Owned
  {
    LoraClientOperationMessage *p;
    ~Owned() { if (p) lora_client_operation_message__free_unpacked(p, NULL); }
    void reset(LoraClientOperationMessage *n)
    {
      if (p) lora_client_operation_message__free_unpacked(p, NULL);
      p = n;
    }
  } owned{rcv_message};

  if (was_encrypted && rcv_message->encrypted)
  {
    LoraClientOperationMessage *decrypted_message = decryptDownlink(rcv_message, outer_header);
    if (decrypted_message == NULL)
      return;
    owned.reset(decrypted_message);

    // P2b: a successful decrypt is PROOF the hub still holds the same base
    // nonce we do — i.e. the resumed session actually works.  This is what
    // cancels the resume fallback.  Deliberately NOT set by a plaintext frame:
    // if the hub lost its state it answers with a plaintext BaseNonceExchange,
    // and in that case we DO want the fallback to fire and re-register.
    this->noteSessionProven();
    this->frame_authenticated_ = true;
  }
  LoraClientOperationMessage *message_to_process = owned.p;

  ESP_LOGI(TAG, "Incoming length: %d", packetSize);
  ESP_LOGI(TAG, "Dest Adreess: %d", outer_header->destaddress);
  ESP_LOGI(TAG, "Dest Subnet: %d", outer_header->destsubnet);
  ESP_LOGI(TAG, "Sender Address: %d", outer_header->senderaddress);
  ESP_LOGI(TAG, "Config Address: %d", sysCtrl->getConfigAddress());
  ESP_LOGI(TAG, "Config Subnet: %d", sysCtrl->getConfigSubnet());

  if (!admitFrame(message_to_process, outer_header, was_encrypted))
    return;

  dispatchCommand(message_to_process, outer_header);
}

// F-39: Old onReceive() removed 2026-05-25 — dead code replaced by onReceiveNew().

// Initialize memory pool and queues
esp_err_t CmdDispatcher::init_memory_pool(void)
{
  // Create queues
  this->rx_free_buffer_queue = xQueueCreate(POOL_SIZE, sizeof(rx_buffer_t *));
  this->rx_data_queue = xQueueCreate(RX_QUEUE_SIZE, sizeof(rx_buffer_t *));

  if (!this->rx_free_buffer_queue || !this->rx_data_queue)
  {
    ESP_LOGE(TAG, "Failed to create queues");
    return ESP_FAIL;
  }

  // Create mutex for pool protection
  rx_pool_mutex = xSemaphoreCreateMutex();
  if (!rx_pool_mutex)
  {
    ESP_LOGE(TAG, "Failed to create mutex");
    return ESP_FAIL;
  }

  // Initialize free buffer queue with all buffers
  for (int i = 0; i < POOL_SIZE; i++)
  {
    rx_buffer_t *buffer = &rx_memory_pool[i];
    if (xQueueSend(this->rx_free_buffer_queue, &buffer, 0) != pdTRUE)
    {
      ESP_LOGE(TAG, "Failed to initialize free buffer queue");
      return ESP_FAIL;
    }
  }

  ESP_LOGI(TAG, "Memory pool initialized with %d buffers", POOL_SIZE);
  return ESP_OK;
}

CmdDispatcher::rx_buffer_t *CmdDispatcher::get_free_buffer(TickType_t timeout)
{
  rx_buffer_t *buffer = NULL;

  if (xQueueReceive(this->rx_free_buffer_queue, &buffer, timeout) == pdTRUE)
  {
    // Clear the buffer
    memset(buffer->data, 0, BUFFER_SIZE);
    buffer->length = 0;
    buffer->timestamp = xTaskGetTickCount();
    return buffer;
  }

  return NULL;
}

// Return a buffer to the free pool
esp_err_t CmdDispatcher::return_buffer_to_pool(rx_buffer_t *buffer)
{
  if (!buffer)
  {
    return ESP_ERR_INVALID_ARG;
  }

  // Validate buffer belongs to our pool
  if (buffer < rx_memory_pool || buffer >= &rx_memory_pool[POOL_SIZE])
  {
    ESP_LOGE(TAG, "Invalid buffer pointer");
    return ESP_ERR_INVALID_ARG;
  }

  if (xQueueSend(this->rx_free_buffer_queue, &buffer, 0) != pdTRUE)
  {
    ESP_LOGE(TAG, "Failed to return buffer to pool");
    return ESP_FAIL;
  }

  return ESP_OK;
}

// F-23: Accessor method — submit a filled rx_buffer to the data queue.
esp_err_t CmdDispatcher::submit_rx_buffer(rx_buffer_t *buf)
{
  if (!buf)
  {
    return ESP_ERR_INVALID_ARG;
  }
  if (xQueueSend(this->rx_data_queue, &buf, 0) != pdTRUE)
  {
    ESP_LOGW(TAG, "RX data queue full, dropping buffer %p", buf);
    return_buffer_to_pool(buf);
    return ESP_FAIL;
  }
  return ESP_OK;
}

void CmdDispatcher::loraCommandProcTask(void *pvParameters)
{
  (void)pvParameters;

  rx_buffer_t *rx_buffer;

  ESP_LOGI(TAG, "Data processing task started");

  while (1)
  {
    // Feed the task watchdog

    // Wait for data buffer
    if (xQueueReceive(this->rx_data_queue, &rx_buffer, portMAX_DELAY) == pdTRUE)
    {
      // Process the data
      ESP_LOGI(TAG, "Processing buffer %p with %d bytes (timestamp: %lu)",
               rx_buffer, rx_buffer->length, rx_buffer->timestamp);

      // Which window caught this frame, for the mark-hit gate in admitFrame.
      // Set here rather than passed down: admitFrame takes no buffer, and this
      // is the same discipline last_rx_len_ already uses — written once per
      // frame, on the task that reads it.
      this->last_rx_window_gen_ = rx_buffer->window_gen;
      onReceiveNew(rx_buffer->data, rx_buffer->length, rx_buffer->rx_us);
      // Simulate processing time
      vTaskDelay(50 / portTICK_PERIOD_MS);

      // Example: Print received data as hex
      ESP_LOG_BUFFER_HEX(TAG, rx_buffer->data, rx_buffer->length);

      // Return buffer to pool after processing
      esp_err_t ret = return_buffer_to_pool(rx_buffer);
      if (ret != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to return buffer to pool");
      }
    }
  }
}

// F-39: stats_task removed 2026-05-25 — dead code, was never spawned.

void CmdDispatcher::enterDeepsleep()
{
  // F-29: On a deep-sleep wakeup the hub re-negotiates via LoginMsg, but we
  //       still persist the final counters/nonce so an unexpected reboot during
  //       the wake window (before login completes) can resume cleanly.
  this->savePersistentState();
  // Mode C's FER: this wake's receive funnel, for the next wake's beacon. The
  // counters live in RAM and start at zero on every boot, so at deep-sleep entry
  // they describe exactly this wake.
  {
    const macfunnel::Counters c = this->macFunnelSnapshot();
    s_prev_wake_windows  = c.windows_armed;
    s_prev_wake_hits     = c.windows_hit;
    s_prev_wake_detected = c.detected;
    s_prev_wake_crcvalid = c.crc_valid;
  }
  ESP_LOGI(TAG, "Entering deep sleep — state persisted; nonce renegotiated on wakeup");
  sysCtrl->enterDeepsleep();
}

// ---------------------------------------------------------------------------
// F-4: Acknowledge a received command.
// ---------------------------------------------------------------------------
void CmdDispatcher::noteAnchorFixed_()
{
  // 0 means never, so a fix at the clock's first microsecond still counts.
  const int64_t now_us = nodeclock::nowUs();
  this->last_anchor_fix_us_ = (now_us > 0) ? now_us : 1;
  this->refused_since_fix_  = 0;
}

// Keeping a kept grid honest, the half the node's own samples cannot do: ask the
// hub for the grid again. See timedmode::syncRequestReason for when and
// GridSyncRequest for why. Measured 2026-09-15 on node 2 (fw 1.0.92): after ~17
// minutes in Mode A the node came back 11 ms off its marks.
void CmdDispatcher::maybeRequestGridSync()
{
  const int64_t now_us = nodeclock::nowUs();
  timedmode::SyncRequestState s;
  s.timed_rx_enabled   = this->timed_rx_enabled_;
  s.grid_active        = this->grid_.active;
  s.in_mode_b          = (this->last_reason_logged_ == (uint8_t) timedmode::Demotion::None);
  // Not "a login happened": the request must go out encrypted, which needs the
  // session state and the base nonce both.
  s.session            = this->canResumeSession();
  s.resync_max_s       = this->grid_.params.resync_max_s;
  s.s_since_anchor_fix = (this->last_anchor_fix_us_ == 0)
                             ? 0xFFFFFFFFu
                             : (uint32_t) ((now_us - this->last_anchor_fix_us_) / 1000000);
  s.refused_since_fix  = this->refused_since_fix_;
  s.s_since_request    = (this->last_sync_request_us_ == 0)
                             ? 0xFFFFFFFFu
                             : (uint32_t) ((now_us - this->last_sync_request_us_) / 1000000);
  const timedmode::SyncRequestReason reason = timedmode::syncRequestReason(s);
  if (reason == timedmode::SyncRequestReason::None)
    return;

  this->last_sync_request_us_ = (now_us > 0) ? now_us : 1;
  if (this->gridsync_requests_ < 0xFFFFFFFFu)
    this->gridsync_requests_++;
  ESP_LOGW(TAG, "   GridSync request: reason %u, anchor %u s old, %u samples refused "
                "(%u since boot)",
           (unsigned) reason, (unsigned) s.s_since_anchor_fix,
           (unsigned) s.refused_since_fix, (unsigned) this->gridsync_requests_);
  this->setStatus(BlindsStatusCmd::SYSCMD_GRIDSYNC_REQUEST, (uint32_t) reason);
}

void CmdDispatcher::sendCommandAck(uint32_t ack_msg_id)
{
  // B4: remember what we just answered, so a retransmit of the SAME command can
  // be answered again instead of dropped as a replay.
  //
  // Recorded here rather than at each of the three call sites (cover op, sysop,
  // schedule) because this is the one place every acked command passes through,
  // so a handler added later cannot forget to do it. Re-acks re-enter this
  // function, so guard against a re-ack resetting the very counters that bound
  // it — note() only on a first acceptance.
  const int64_t now_ack_us = nodeclock::nowUs();
  // note() is idempotent per msgid now that the cache holds several, so the
  // old guard against a re-ack refreshing its own budget lives inside it.
  this->ack_cache_.note(ack_msg_id, now_ack_us);

  // F-4: msgid travels with the queued command (tx_command_t.arg), so it can no
  // longer be overwritten by a concurrent dispatch before the TX task reads it.
  this->setStatus(BlindsStatusCmd::SYSCMD_ACK, ack_msg_id);
}

// ---------------------------------------------------------------------------
// F-5: NVS-backed persistence of counter + base-nonce state.
// ---------------------------------------------------------------------------
void CmdDispatcher::loadPersistentState()
{
  this->session_.load();
  // destAddress is routing state and stays here; NOT truncated to uint8_t —
  // see the member comment (F-4).
  if (this->session_.hasValidState())
    this->destAddress = this->session_.persistPeer();
}

void CmdDispatcher::savePersistentState()
{
  this->session_.save();
}

void CmdDispatcher::maybePersist_()
{
  this->session_.maybePersist();
}

bool CmdDispatcher::checkQueuesIdle()
{

  if (this->rxCmdQueueNew == NULL || this->txCmdQueueNew == NULL || this->sysCmdQueueNew == NULL || motCtrl->motorCmdQueueNew == NULL || motCtrl->motCmdQueueNew == NULL)
  {
    ESP_LOGE(TAG, "Queues not initialized, cannot enter deep sleep");
    return true;
  }
  UBaseType_t numRx = uxQueueMessagesWaiting(this->rxCmdQueueNew);
  UBaseType_t numTx = uxQueueMessagesWaiting(this->txCmdQueueNew);
  UBaseType_t numSys = uxQueueMessagesWaiting(this->sysCmdQueueNew);
  UBaseType_t numMot = uxQueueMessagesWaiting(motCtrl->motorCmdQueueNew);
  UBaseType_t numMotCmd = uxQueueMessagesWaiting(motCtrl->motCmdQueueNew);

  if (numRx + numTx + numSys + numMot + numMotCmd > 0)
  {
    printf("Still tasks in queues: Rx: %d Tx: %d, Sys %d , Mot %d, MotCmd %d\n", numRx, numTx, numSys, numMot, numMotCmd);
    return false;
  }

  // Queue depth alone is not "the motor has finished". It held the node awake
  // through a move only because the command happens to stay queued for the
  // duration - a property of MotorCtrl's queue discipline, not a guarantee.
  // Ask the FSM directly, whose IDLE -> non-IDLE / non-IDLE -> IDLE transitions
  // are the same ones that acquire and release motorLock, so the invariant
  // survives a refactor: deep-sleeping mid-travel would leave the blind half
  // closed and the stored position wrong.
  if (motCtrl->isBusy())
  {
    printf("Motor still running - deferring deep sleep\n");
    return false;
  }

  // F-41: a battery measurement queued or in progress also counts as busy.
  // Sleeping now would drop the post-move battery update, and the node would
  // then keep reporting the previous load-sagged value on its next wake — the
  // cache is echoed by every battery AND position frame until the next
  // measurement, so cutting one short is not a lost sample but a wrong value
  // repeated.
  if (motCtrl->motorBatteryQueue != NULL && uxQueueMessagesWaiting(motCtrl->motorBatteryQueue) > 0)
  {
    printf("Battery measurement pending\n");
    return false;
  }
  if (this->batteryMeasurementBusy_)
  {
    printf("Battery measurement in progress\n");
    return false;
  }

  return true;
}
