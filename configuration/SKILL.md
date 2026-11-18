---
name: lora-blinds
description: Where to start when working on the LoRa blinds system (ESPHome hub in this repo, ESP-IDF node firmware in BlindsESP): current state, procedures, wire format. Use before changing local_components/, loradevices.yml, blinds.proto or the node firmware.
---

# LoRa blinds system: start here

Two repositories:

- **Hub** (this repo, `c:\Development\esphome\configuration\`): ESPHome configuration `loradevices.yml`
  and the custom components in `local_components/` (`lora_tracker` = MAC-0 radio side, `lora_client` =
  per-node listener, MAC-1/MAC-2 and the application, `loracover`, `blindsproto`). Mains powered.
- **Node** (`C:\Development\PlatformIO\PlatformIO\BlindsESP\`): battery-powered ESP-IDF firmware of
  each blind. It is NOT the hub.

Read, in this order:

1. `docs/project-state.md`: the current state, deliberate protocol behaviours that must not be
   "fixed", hard constraints, the open issues and where everything is.
2. `docs/rollout-runbook.md`: build, test (proto_sim), flash, OTA, bench campaigns and the traps.
3. `docs/mac-wire-format.md`: MAC wire v1, the only wire (fixed header, ports, AUTH, CMAC-8 tags).
   The v0 envelope (`LoraHeader`, `EncryptedPayload`, AES-GCM, `CommandAck`, `BaseNonceExchange`) is
   deleted; its names and numbers are reserved in `blinds.proto`.

Rules that apply to every change:

- `blinds.proto` lives in the node repo (`proto/blinds.proto`); regenerate both stub copies with
  `proto/regen_stubs.sh` and ship both firmwares together. Never renumber a field; remove and reserve.
- Never change the node NVS layout or `kRestoreStateVersion`.
- Hub host tests: `tests/proto_sim` (gtest, built in WSL against a clean snapshot of the node tree).
- Do not flash or push unless the owner asks.
