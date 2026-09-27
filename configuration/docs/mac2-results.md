# MAC-2 (crypto) across A, B and turnaround — 2026-09-27

**Build under test:** node 2 fw 1.1.7 (adds the `cryptoRequired()` wiring below;
otherwise unchanged from 1.1.6), hub = the 2026-09-27 encrypted-MAC-ping commit
on the bench image (`tools/make_bench_yaml.py`, plus two `MAC-1 + MAC-2` long
buttons and an encrypted-ping-start button). Production image restored
afterwards (config hash `0x346150ef`, confirmed). Node 1 unaffected (still
fw 1.1.4 as of this session; not part of MAC-2 measurement). Raw captures:
`logs/20260927_103146_mac2b/` (git-ignored; a first attempt in
`20260927_102251_mac2` is discarded — see "A hub double-boot mid-run" below).

MAC-2 = AEAD (AES-GCM) encrypt/decrypt, MIC check, base-nonce session. Method
(mac-layer.md §4): the same grid, counter AND crypto both ON, 900 s, production
power profile, one press each — compared against the MAC-1-only numbers taken
2026-09-26.

## Two defects fixed before this could be measured at all

Same shape as MAC-1's turnaround gap two days ago: **the mechanism existed and
had literally never run.**

1. **`cryptoRequired()` was dead code.** `MacSublayers.h` defines it
   (`is_mac_control ? cfg.crypto_enabled : true`) and `mac_sublayers_test.cpp`
   exercises it as a pure function — but nothing in `CmdDispatcher.cpp` ever
   called it. A plaintext MAC ping was accepted by the node's plaintext gate
   regardless of the crypto switch. Fixed: the ping exemption
   (`ping_during_test`, added for MAC-1's turnaround fix) now also requires
   `!cryptoRequired(...)`, i.e. MAC-2 off. With MAC-2 on, only an **encrypted**
   ping reaches MAC-0 — the same way every other downlink already does.
2. **The hub's MAC ping had no encryption option at all.**
   `build_mac_ping_frame_()` packed raw, unconditionally. `start_mac_ping()`
   gained a `crypto` argument; when set (and a session is confirmed) it
   **reserves a block of the session's own tx-msgid space up front** (one NVS
   write, in the button-press/main-loop call) and the ping's `header.msgid`
   comes from that block — not from the ping's own dedicated counter, which
   would risk reusing a msgid (and therefore an IV) already spent on ordinary
   traffic under the same base nonce. Falls back to plaintext, logged, if no
   session exists — the safe direction, matching every other downlink.

Both fixed and host-tested red-first / mutation-killed (12 node-side mutants
across the ping-crypto gate and hub-side reservation mechanism; see commit).

## A hub double-boot mid-run (discarded, not a code defect)

The first attempt (`20260927_102251_mac2`) produced a contaminated run: the
hub rebooted a second time moments after the bench OTA (visible as a second
`startup: broadcast grid demote sent` + `NVS restore failed or version
mismatch (expected v2) — starting fresh, waiting for REGISTER` for node 2 at
10:23:28, ~50 s after the upload completed), and node 2's session was never
rebuilt afterwards — nothing prompts a **registered** node to re-send REGISTER
on its own, so the hub sat in `registered=no` while the node believed it still
held a session. Recognised from `MAC ping: no confirmed session` on the hub and
confirmed as a false run once decrypt-side rejections showed the ModeTest
START itself going out in the clear. **Recovery: `esptool ... hard-reset`
on the node** (COM6), which forces a fresh REGISTER the hub is waiting for —
re-onboarded in 9 s. Re-ran the whole campaign afterwards; not a code issue on
either side, and the mechanism this exposed (hub NVS restore failing on its own
reboot) is a separate, already-known class — see `docs/hub-onboarding.md`.

## Mode A (grid 1093 ms, 1 copy), counter + crypto both ON

| | MAC-0 (2026-09-26) | MAC-1 only (2026-09-26) | MAC-1 + MAC-2 (today) |
|---|---|---|---|
| mode / refusal | 1 / 0 | 1 / 0 | 1 / 0 |
| crcValid → addressed | 56 → 56 | 59 → 59 | 54 → 53 |
| **true FER** | **0 of 56** | **0 of 59** | **0 of 54** |
| MIC valid / addressed | — | — | 53 / 53 |
| raw ppm (n) | +10 (55) | +9 (58) | +9 (53) |

The one crcValid-but-not-addressed frame in the MAC-2 run is ordinary
cross-talk (another node's traffic passing CRC), the same shape seen in every
other run — not a MAC-2 effect. Every addressed, counter-accepted frame also
passed its MIC: MAC-2 is not merely switched on, it is decrypting correctly on
real hardware for the first time this session.

## Mode B (grid 1500 ms, 1 copy, promoted), counter + crypto both ON

| | MAC-0 | MAC-1 only | MAC-1 + MAC-2 |
|---|---|---|---|
| windows armed / hit (WMR) | 595/594 (1680 ppm) | 598/598 (0) | 594/592 (3367 ppm) |
| HW-8: one-shot n ≥ armed | 598 ≥ 595 | 601 ≥ 598 | 597 ≥ 594 (0 missed) |
| **true FER** | 0 of 601 | 0 of 601 | **0 of 600** |
| raw ppm / **residual** | +9 / **−1** | +9 / **0** | +9 / **0** |
| phase p50/p99/max (µs) | 659/2821/2921 | −48/919/1596 | 412/1984/2026 |
| MIC valid / addressed | — | — | 597 / 600 |

All three pass the Mode B line (\|residual\| < 20 ppm, mode 2, 0 missed
one-shots, ≥30 samples over ≥200 s). WMR moves between runs (1680 → 0 → 3367
ppm) with no trend across the sublayer switches — 1–2 misses out of ~594–598
windows is ordinary RF variance, not attributable to MAC-2: an added decrypt
step cannot cause a window miss, only add to turnaround, which is measured
separately below. 3 frames were counter-accepted-but-not-MIC-valid (597 vs
600) — beacons, exactly as MAC-1's accounting predicted (broadcast, no
per-node MIC).

## Turnaround (MAC ping running inside a 900 s Mode A run)

| sublayers | turnaround p50 / p99 (µs) | n |
|---|---|---|
| MAC-0 (2026-09-26) | 6 912 / 7 675 | 52 |
| +MAC-1 (2026-09-26) | 6 926 / 7 660 | 47 |
| +MAC-1+MAC-2 (today) | **9 860 / 10 265** | 51 |

**MAC-2's own cost, isolated by subtraction: ≈ 2.9 ms** (9 860 − 6 926 µs) —
the AES-GCM decrypt + MIC check the node now performs on every accepted MAC
control frame under this sublayer. MAC-1's own cost stays what it was
(≈ 14 µs, noise). Total turnaround with everything on, **9.86 ms**, remains
well inside every ceiling in the plan: the assumed 20 ms budget, the k+2 ack
row's 36.5 ms break point, the k+3 headline's 62.9 ms ceiling, and
`kUplinkOffsetUs` = 60 ms.

Same caveats as the MAC-1 write-up: n ≈ 50–55 per run (free-running Mode A
catch rate), so the median is well supported and the p99 less so; running
pings inside the test perturbs the raw ppm figure (read separately from
ping-free runs); this measures one frame length (a bare ping) — the
`payloadPadTo` sweep is still unimplemented.

## Reading

* **MAC-2 costs ~2.9 ms of turnaround and nothing else measured** — no FER
  effect, no residual/ppm effect, no window-miss effect. The layering-proposal
  attribution method (MAC-0 → +MAC-1 → +MAC-1+MAC-2, each a measured delta)
  now has real numbers for both sublayers.
* **Every MIC check passed.** With MAC-2 live on hardware for the first time,
  nothing suggests a key, AAD, or nonce-derivation defect — `MIC_FAIL 0` in
  both modes, across two runs.
* Session-management housekeeping needed for the measurement (the reserved
  msgid block) is a hub-only mechanism with no wire or protocol change; it
  borrows the session's own sequence space rather than adding a new one.

## Next

MAC-2 was the last sublayer in the plan's staging (`mac-layer.md` §4:
MAC-0 → MAC-1 → MAC-2). Per the layering rule, the next step is the
application/session layer work already flagged as open: the promoted-quiet-
node visibility gap, and the header-timing-fields-not-authenticated finding
(review #2) — neither is a sublayer switch, both are design decisions.
