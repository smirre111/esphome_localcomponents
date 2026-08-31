# ESPHome LoRa Blinds System — Expert Skill

This skill activates automatically when working in:
- `c:\Development\esphome\configuration\` (YAML configs or `local_components/`)
- `C:\Development\PlatformIO\PlatformIO\BlindsESP\` (hub firmware)
- Any question about the LoRa blinds protocol, `blinds.proto`, or protobuf-c interop

---

## System Architecture

```
Home Assistant
     │  ESPHome native API
     ▼
┌─────────────────────────────────────────────────┐
│  loradevices hub (ESP32, ESP-IDF, ESPHome)          │
│                                                 │
│  lora_tracker  ◄── SX1278 LoRa radio            │
│      │  433 MHz, SF7, BW500, syncword=0x12      │
│      │                                         │
│  lora_client (rol_1, rol_2, …)                  │
│      │  short_addr / subnet / MAC / sleep       │
│      │                                         │
│  loracover  /  lorasensor  /  OTA-button        │
└─────────────────────────┬───────────────────────┘
                          │ LoRa RF (protobuf-c packets)
                          │
┌─────────────────────────▼───────────────────────┐
│  BlindsESP node  (ESP-IDF)                      │
│  C:\Development\PlatformIO\PlatformIO\BlindsESP  │
│                                                 │
│  LoraInterface → SX1278 (CAD + RX/TX tasks)     │
│  CmdDispatcher → decodes proto, drives           │
│                  MotorCtrl + SystemCtrl          │
└─────────────────────────────────────────────────┘
```

---

## Key File Locations

| Path | Purpose |
|------|---------|
| `loradevices.yml` | ESPHOME HUB Top-level ESPHome config |
| `local_components/lora_tracker/` | ESPHOME HUB Radio driver + base classes (LORATracker, LORAListener, LORAClient) |
| `local_components/lora_client/` | ESPHOME HUB Per-device client (LORAListener::set_response, send_login, etc.) |
| `local_components/loracover/cover/` | ESPHOME HUB Cover entity (open/close/stop/position) |
| `local_components/loracover/sensor/` | ESPHOME HUB Battery voltage & level sensors |
| `local_components/loracover/button/` | ESPHOME HUB OTA trigger button |
| `local_components/blindsproto/blinds.pb-c.{c,h}` | **Generated** protobuf-c stubs — do not edit by hand |
| `local_components/lora_tracker/proto/blinds.proto` | ESPHome-side copy of the proto |
| `C:\Development\PlatformIO\PlatformIO\BlindsESP\proto\blinds.proto` | **Authoritative proto** (hub owns it) |
| `BlindsESP\main\CmdDispatcher.cpp` | esp-idf node receive/decode/dispatch logic |
| `BlindsESP\main\LoraInterface.cpp` | esp-idf node  radio init, TX/RX/CAD tasks |

---

## Protobuf Protocol — `blinds.proto`

> **The hub's `proto/blinds.proto` is the single source of truth.**
> The ESPHome copy and generated `blinds.pb-c.{c,h}` MUST stay byte-for-byte compatible.

### Node (esp-idf) → Hub with ESPHome (`LoraClientResponseMessage`)

```protobuf
oneof proto {
  ClientAvailable avail      // wake-up ping (available=true)
  ClientRegister register    // first boot: MAC → triggers ClientConfig push
  ClientBattery state        // voltage (float V)
  CoverPosition position     // position [0..1], voltage, current
  LoginMsg login             // challenge nonce
  EncryptedPayload encrypted // AEAD wrapper when header.encrypted=1
}
```

### Hub with ESPHome  → Node (esp-idf)Hub (`LoraClientOperationMessage`)

```protobuf
oneof cmd {
  LoraCoverOperation operation  // CMD_OPEN/CMD_CLOSE/CMD_STOP or position float
  ClientOperation sysop         // CMD_OTA/CMD_SLEEP/CMD_STATUS/CMD_ENABLE_WIFI/…
  ClientConfig clientconfig     // mac_addr, addr, subnt, name, sleepDuration
  CoverConfig coverconfig       // openTime, closeTime (seconds)
  LoginMsg login                // nonce reply
  BaseNonceExchange basenonce   // 4-byte per-peer base nonce for AEAD
  EncryptedPayload encrypted    // AEAD wrapper when header.encrypted=1
}
```

### LoraHeader

| Field | Meaning |
|-------|---------|
| `destAddress` | Short address of target (e.g. 17) |
| `destSubnet` | Subnet (e.g. 2) |
| `senderAddress` | 0xFF = hub/controller; client short addr on responses |
| `msgId` | Monotonically increasing — replay protection |
| `encrypted` | 1 = payload in EncryptedPayload field |

---

## Radio Parameters — Both Sides Must Match

| Parameter | Value |
|-----------|-------|
| Frequency | 433.05 MHz + 250 kHz = 433.3 MHz |
| Spreading Factor | 7 |
| Coding Rate | 4/8 |
| Signal Bandwidth | 500 kHz |
| Sync Word | 0x12 |
| CRC | Enabled |
| Preamble (TX/RX) | 8 symbols |

Any mismatch → complete communication failure.

---

## Security / AES-GCM-128 Encryption

Both sides share the same implementation:

| Element | Value / Method |
|---------|---------------|
| Key | SHA-256(`"LoRaKey1"`)[0:16] — both sides use `psa_hash_compute(PSA_ALG_SHA_256, …)` |
| IV (12 bytes) | `base_nonce_BE[4] ‖ counter_BE[8]`, where `counter = LoraHeader.msgid` on **uplink** (node→hub) and `msgid \| (1ULL << 63)` on **downlink** (hub→node). That direction bit (`kDownlinkNonceFlag`, defined identically in `lora_client.cpp` and `CmdDispatcher.cpp`) is what stops the two directions reusing an IV under the shared per-peer base nonce. Omitting it fails the tag check with otherwise-correct key/AAD/base-nonce — a confusing failure, so check it first. |
| AAD (16 bytes) | `destaddress‖destsubnet‖senderaddress‖msgid`, each 4-byte BE. The old 5th field (the `encrypted` header flag) was removed — encryption is inferred from the oneof case. AAD deliberately excludes `burstIndex`/`burstCount` so the tracker's per-copy burst re-stamping does not invalidate the tag. |
| Tag | **8 bytes** — truncated (`kAesGcmTagBytes`) for the slim on-air envelope, not the full 16 |
| Ciphertext | **Payload only.** The inner message's header is stripped before encryption; the receiver uses the plaintext outer header. |
| Nonce provisioning | **Primary:** ESPHome embeds `base_nonce = esp_random()` in `LoginMsg.nonce` (uint32). **Recovery only:** standalone `BaseNonceExchange` message. |
| Replay protection | `LoraHeader.msgid` is the **unified counter** — the same value is used for both replay checks (`msgid > last_seen`) and AES-GCM nonce derivation. No separate frame-counter maps exist. |

When `header.encrypted=1`, the `cmd`/`proto` oneof carries `EncryptedPayload`; the inner protobuf is decrypted and re-parsed.

**Nonce lookup asymmetry (by design):** ESPHome stores `base_nonce_map[node_short_addr]`; the node stores `peer_counters[hub_sender_addr=0xFF].base_nonce`. Different lookup keys, same stored value — nonce derivation produces identical IV on both sides.

---

## Class Hierarchy (ESPHome side)

```
esphome::lora_tracker
├── LORATracker    : Component
│     register_client(LORAClient*)
│     send(buf, len)  ← sends via SX1278
│     receive() / checkReception()
│
├── LORAListener   : EntityBase, Component
│     short_address_, subnet_address_, sleep_duration_
│     frame_counter_ {rx_message_id, tx_message_id}  ← persisted to flash
│     registered_   ← set after ClientRegister ACK
│     set_response(data, len)  → unpack LoraClientResponseMessage,
│                                handle encryption, dispatch to nodes_[]
│     send_login(), send_remote_config(), enterSleep(), triggerOTA()
│     incrTxMessageId()  → increments + saves to ESPPreference
│
└── LORAClient : LORAListener   (thin subclass, adds app_id)

LORAClientNode  (abstract interface)
    set_response(data, len)    ← overridden by cover/sensor/button
    set_lora_client_parent(LORAListener*)

esphome::loracov
└── LoraCoverComponent : Cover, LORAClientNode, Component
      control() → sends LoraCoverOperation via parent_->parent_->send()
      set_response() → handles CoverPosition, publishes HA state
      send_remote_config() → sends CoverConfig(openTime, closeTime)
```

---

## Session / Registration Lifecycle

1. **Hub boots** → sends `ClientRegister{mac_addr}` (broadcast or known addr)
2. **ESPHome** `LORAListener::set_response()` gets `PROTO_REGISTER`, verifies MAC, calls `send_remote_config()`
3. `send_remote_config()` sends `ClientConfig{mac_addr, addr, subnt, name, sleepDuration}`
4. **Hub** `CmdDispatcher::onReceiveNew()` receives `CMD_CLIENTCONFIG`, validates MAC, calls `sysCtrl->setAddress/Hostname/SleepDuration/setRegistered()`, saves to **LittleFS**
5. **ESPHome** `LoraCoverComponent::set_response()` gets `PROTO_REGISTER` → sends `CoverConfig{openTime, closeTime}`
6. **Hub** `CMD_COVERCONFIG` → `sysCtrl->setTimes()`, saves to LittleFS
7. **Login challenge**: **ESPHome** (hub) calls `send_login()` → generates `base = esp_random()`, resets its own `frame_counter_.tx/rx = 0`, stores `s_base_nonce_map[node_addr] = base`, sends `LoginMsg{nonce=base}` with `header.encrypted=0`. **Node** (`CMD_LOGIN`) resets `tx_message_id_=0` and `rx_message_id_=0`, calls `set_base_nonce(sender_addr, login->nonce)`. No reply from node. Hub's first post-login message uses `msgid=2` (login used 1); node's first response uses `msgid=1`. Both `> 0` so replay checks pass.
8. **Normal ops**: `LoraCoverOperation` (open/close/stop/position), `CoverPosition` responses, `ClientBattery`
9. **Sleep**: YAML `on_time` fires `loracover.on_sleep_start: rol_1` → `LORAListener::enterSleep()` → sends `CMD_SLEEP`, arms fallback login timer at `sleep_duration + 5 s boot margin + short_address × 3 s stagger`
10. **OTA**: `trigger_ota` button → `LORAListener::triggerOTA()` → sends `CMD_OTA` → hub reboots into OTA mode

---

## ESPHome YAML Patterns (`loradevices.yml`)

### LoRa Hub
```yaml
lora_tracker:
  id: my_lora_tracker
```

### Client Node
```yaml
lora_client:
  - id: rol_1
    name: "RollladenWohnzimmer1"
    time_id: homeassistant_time       # must be homeassistant time platform
    short_address: 17                 # must match hub's NVS-stored address
    subnet_address: 2
    sleep_duration: 21600             # seconds (6 h)
    mac_address: e0:8c:fe:5f:b7:a4   # must match hub's efuse MAC exactly
```

### Cover
```yaml
cover:
  - platform: loracover
    lora_client_id: rol_1
    name: "RollladenWohnzimmer1"
    id: rol_1_cover
    invert_position: false
    open_duration: 60     # → CoverConfig.openTime
    close_duration: 60
```

### Battery Sensors
```yaml
sensor:
  - platform: loracover
    lora_client_id: rol_1
    battery_level:
      name: "... Battery Level"
    battery_voltage:
      name: "... Battery Voltage"
```

### OTA Button
```yaml
button:
  - platform: loracover
    lora_client_id: rol_1
    name: "Trigger OTA1"
```

### Scheduled Sleep
```yaml
time:
  - platform: homeassistant
    id: homeassistant_time
    on_time:
      - seconds: 0
        minutes: 0
        hours: 23
        days_of_week: MON-SUN
        then:
          - loracover.on_sleep_start: rol_1
```

---

## Regenerating Protobuf-C Stubs

**`protoc-c` is NOT installed on Windows — use WSL** (`protobuf-c 1.5.2`, `protoc 3.21.12`).
Regenerating from the authoritative proto reproduces the committed stubs byte-for-byte,
so this workflow is safe:

```bash
wsl.exe -e bash -lc '
SRC=/mnt/c/Development/PlatformIO/PlatformIO/BlindsESP/proto
cd "$SRC"
protoc-c --c_out=. blinds.proto          # regenerates blinds.pb-c.{c,h} in place
'
```

**The generated stubs live in FOUR places — all must be updated together:**

| Location | Used by |
|----------|---------|
| `BlindsESP/proto/blinds.pb-c.{c,h}` | source of truth (regenerate here) |
| `BlindsESP/components/blinds/src/blinds.pb-c.c` + `components/blinds/include/blinds.pb-c.h` | **the node build** (`main` REQUIRES the `blinds` component — this is the `.c` that actually links) |
| `BlindsESP/main/include/blinds.pb-c.h` | node include copy |
| `esphome/configuration/local_components/blindsproto/blinds.pb-c.{c,h}` | ESPHome (hub) build |

```bash
# distribute after regen
SRC=/mnt/c/.../BlindsESP/proto
cp "$SRC/blinds.pb-c.c" /mnt/c/.../BlindsESP/components/blinds/src/blinds.pb-c.c
cp "$SRC/blinds.pb-c.h" /mnt/c/.../BlindsESP/components/blinds/include/blinds.pb-c.h
cp "$SRC/blinds.pb-c.h" /mnt/c/.../BlindsESP/main/include/blinds.pb-c.h
cp "$SRC/blinds.pb-c.c" /mnt/c/.../esphome/configuration/local_components/blindsproto/blinds.pb-c.c
cp "$SRC/blinds.pb-c.h" /mnt/c/.../esphome/configuration/local_components/blindsproto/blinds.pb-c.h
```

> Forgetting `components/blinds/` gives a **linker** error (`undefined reference to <msg>__descriptor`)
> even though everything compiles — the node links the stale `.c` from that component.

**Rules for proto changes:**
- Never renumber existing fields
- New fields go in existing `oneof` blocks only (e.g. `CommandAck ack = 15` in the response oneof)
- Update **both** `.proto` copies + **all four** generated stub locations atomically
- Regenerate stubs + rebuild + deploy both firmwares together

---

## Common Issues & Diagnostics

### No communication at all
- Verify radio params match (freq, SF, BW, sync word — both compiled-in constants)
- Verify `short_address` + `subnet_address` match hub's LittleFS config
- Verify `mac_address` in YAML matches `esp_read_mac(ESP_MAC_EFUSE_FACTORY)` on hub

### Login never sent — no log output from `do_login_and_arm_retry_()`

Almost always means `parent_` is `nullptr`. Root cause: `LORATracker::register_client()` must call `client->set_parent(this)` **before** pushing to `clients_`. Without it the guard `if (this->parent_ == nullptr) return;` silently swallows every attempt.

Secondary cause: NTP `on_time_sync_callback` using `login_acked_` as the guard instead of `startup_login_initiated_`. A node sending encrypted messages before NTP sync sets `login_acked_=true`, causing the callback to skip `schedule_startup_login_()` entirely and leaving `s_base_nonce_map` empty after reboot.

### `Login not acknowledged — retry N/24` despite node logging `CMD_LOGIN`

The node processed the login correctly but sent no response. The hub sets `login_acked_` only when it receives **any** valid (msgid-increasing) message from the node. Fix: `CMD_LOGIN` handler in `CmdDispatcher.cpp` must call `this->sendAvailable()` after storing the base-nonce. `processTxCommand` assigns `msgid=1` (first post-reset tx), encrypts with the fresh nonce, and the hub's `set_response()` sees `msgid=1 > rx=0`, sets `login_acked_=true`, and cancels the retry interval.

### Login startup timer — per-node stagger required

All `set_timeout("login_startup", delay, …)` calls must add `short_address_ × 3000 ms` stagger to prevent simultaneous transmissions when multiple nodes are awake at hub reboot. 3 s per address unit covers a full challenge + response exchange at SF12 worst case (~2.5 s airtime per packet). **Exception:** the REGISTER-triggered 500 ms timer — REGISTER events from different nodes arrive at different times and are naturally serialized.

### `duplicate or old message ID` log flood
- Normal after reboot — login challenge resets both counters
- `LORAClientRestoreState` persists counters to ESPPreference flash
- Hub counters (`tx_message_id_`, `rx_message_id_` in `CmdDispatcher`) are **RAM-only** — reset on reboot
- Fix: ensure login challenge completes successfully after every reboot

### AES-GCM decryption fails
- Verify both sides use the same key derivation: SHA-256(`"LoRaKey1"`)[0:16]
- Verify nonce structure: `base_nonce_BE[4] || (uint64_t)msgid_BE[8]`
- Verify AAD built from header fields in same order (see `build_header_aad` in both sources)
- Check login completed: `LoginMsg.nonce` must have been received and stored by the node (`CMD_LOGIN`). If the base-nonce map is empty after hub reboot (it is never persisted), login must fire before any encrypted packet arrives.

### `Could not read protobuf`
- Proto schema mismatch — re-sync and regenerate `blinds.pb-c.{c,h}`
- Packet truncation: ensure `BUFFER_SIZE 256` ≥ actual packet size

### Client never registers
- MAC in YAML must match hub exactly (use `esp_read_mac(ESP_MAC_EFUSE_FACTORY)` output)
- Broadcast address is `0xFF`; subnet broadcast is `0xFE`
- Hub must be already configured and running

### Cover position not updating in HA
- Hub sends `CoverPosition` only after motor movement or `CMD_STATUS`
- Check `CmdDispatcher::sendPosition()` is invoked
- Verify `LoraCoverComponent::set_response()` receives `PROTO_POSITION`

---

## Important Constraints

| Constraint | Detail |
|-----------|--------|
| ESP32 variant | `esp32: variant: esp32` (not S2/S3/C3) |
| Framework | `esp-idf` (not Arduino) — BLE + UART APIs differ |
| `CONFIG_BT_ENABLED` | Auto-set by `lora_tracker/__init__.py → to_code()` |
| Flash write throttle | `preferences: flash_write_interval: 30min` reduces wear |
| Max packet | 255 bytes (SX1278 limit); `BUFFER_SIZE 256` accommodates it |
| Config persistence (hub) | **LittleFS** — always `mountLittleFS()` → `saveConfiguration()` → `unmountLittleFS()` |
| Crystal frequency | `CONFIG_XTAL_FREQ_26: y` in `sdkconfig_options` (26 MHz XTAL on this board) |

---

## Development Workflow

1. **Proto change**: edit hub proto → copy to ESPHome → regenerate stubs → build + deploy both
2. **ESPHome component change**: edit `.cpp`/`.h`/`__init__.py` → `esphome compile loradevices.yml` → `esphome upload loradevices.yml`
3. **Node (BlindsESP) change**: edit source → build + flash via `idf.py` (see *Flashing and Monitoring the Node* below)
4. **LoRa OTA to hub**: click "Trigger OTA" button in HA → hub reboots into WiFi OTA mode

---

## Flashing and Monitoring the Node (BlindsESP)

The node is a native **ESP-IDF** project (not PlatformIO/Arduino despite the folder name).

| Item | Value |
|------|-------|
| Project root | `C:\Development\PlatformIO\PlatformIO\BlindsESP` |
| ESP-IDF location | **`C:\Development\esp\v6_0\v6.0\esp-idf` (v6.0)** — the project's managed component `espressif/esp_sysview` requires idf ≥ 6.0, so the **5.5** install at `C:\Development\esp-idf` fails dependency resolution at CMake configure. Use 6.0. |
| Node serial port | **COM6** — node 2 (FTDI `VID_0403+PID_6010`) |
| Hub serial port | **COM9** — ESPHome hub (Silicon Labs CP210x `VID_10C4&PID_EA60`) |

> ⚠️ **Do not infer which port is which from the USB chip.** The intuition
> "CP210x = the ESP32 node, FTDI = something else" is backwards here: the
> **CP210x is the HUB** and the **FTDI is node 2**. Enumerating the ports and
> guessing from the adapter name has produced a confident, wrong answer
> (2026-08-30) even though this table was already correct — a hub flashed onto
> the node's port would be a genuinely expensive mistake. Read this row; do not
> re-derive it. The USB VID/PIDs above are the reliable discriminator if the
> mapping ever needs re-checking.
>
> Sanity check before any flash: the hub answers on HTTP
> (`curl -s -o /dev/null -w "%{http_code}" http://192.168.178.91/`), the node
> does not.

> The default Windows `python` is 3.13, which matches the `idf6.0_py3.13_env` venv.
> Building against the 5.5 install errors with *"no versions of espressif/esp_sysview match … requires idf (>=6.0)"*.

### Activate ESP-IDF environment (required once per PowerShell session)

```powershell
. "C:\Development\esp\v6_0\v6.0\esp-idf\export.ps1"
```

### Build + Flash + Monitor (standard workflow)

```powershell
. "C:\Development\esp\v6_0\v6.0\esp-idf\export.ps1"
# -C avoids needing to cd into the project dir
idf.py -C "C:\Development\PlatformIO\PlatformIO\BlindsESP" -p COM6 flash monitor
```

`flash monitor` builds if needed, flashes, then opens the serial monitor immediately on the same port. Exit with **`Ctrl+]`**.

### Monitor only (node already running)

```powershell
. "C:\Development\esp-idf\export.ps1"
cd "C:\Development\PlatformIO\PlatformIO\BlindsESP"
idf.py -p COM6 monitor
```

### Build only (no flash)

```powershell
idf.py build
```

### Useful monitor options

```powershell
# Set baud rate explicitly if auto-detection fails (default is 115200)
idf.py -p COM6 -b 115200 monitor

# Print timestamps on each log line
idf.py -p COM6 monitor --timestamps
```

---

## Compiling the ESPHome Firmware

### Prerequisites

The ESPHome virtual environment must be activated before every compile. The venv lives at:

```
C:\Development\esphome\git\esphome\venv\Scripts\activate.bat
```

### Step-by-step (PowerShell)

```powershell
# 1. Activate the ESPHome venv
& "C:\Development\esphome\git\esphome\venv\Scripts\activate.bat"

# 2. Compile (from the configuration directory, or use the full path)
esphome compile configuration\loradevices.yml
```

Or as a single one-liner from any working directory:

```powershell
& "C:\Development\esphome\git\esphome\venv\Scripts\activate.bat"; `
  esphome compile "c:\Development\esphome\configuration\loradevices.yml"
```

### Upload / flash

```powershell
esphome upload configuration\loradevices.yml
```

### Known build quirks

| Issue | Symptom | Fix |
|-------|---------|-----|
| **Python 3.13 / cp311 binary incompatibility** | `ImportError: cannot import name 'lfs' from 'littlefs'` during SCons | Download the cp313 wheel (`pip download --platform win_amd64 --python-version 313 --only-binary=:all: littlefs-python==0.17.1`), extract `lfs.cp313-win_amd64.pyd`, copy it to `C:\Users\reinh\.platformio\penv\Lib\site-packages\littlefs\` |
| **Read-only build artefacts** | `Permission denied` copying headers into `.esphome\build\loradevices\src\` | `attrib -R /S "C:\Development\esphome\configuration\.esphome\build\loradevices\src\*"` to clear read-only flags |
| **PSA Crypto not found** | `psa/crypto.h: No such file` | Ensure `CONFIG_MBEDTLS_PSA_CRYPTO_C=y` is present in `sdkconfig_options` in `loradevices.yml` |
| **Framework reinstall on first run** | PlatformIO re-downloads ESP-IDF 5.5.2 (~1 GB) + toolchain | Normal on a clean cache or version bump — wait ~30 min |

### Build output locations

| Artefact | Path |
|----------|------|
| OTA binary | `.esphome\build\loradevices\.pioenvs\loradevices\firmware.ota.bin` |
| Factory (full flash) binary | `.esphome\build\loradevices\.pioenvs\loradevices\firmware.factory.bin` |
| ELF (debug symbols) | `.esphome\build\loradevices\.pioenvs\loradevices\firmware.elf` |
