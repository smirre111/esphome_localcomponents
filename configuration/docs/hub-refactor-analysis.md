# Hub refactoring & optimisation analysis

Measured 2026-08-29, after the node-side refactor. Same instruments used on the
node: `lizard` for complexity, the `proto_sim` harness to judge what is actually
covered, and direct reading for duplication.

**Headline: the hub is in much better shape than the node was.** One function
above CCN 20 versus the node's five, an average CCN of 3.3 across 158 functions,
and no 2000-line files. The work worth doing is not bulk complexity reduction —
it is one genuine correctness risk and a handful of contained cleanups.

---

## 1. The wire format is implemented FOUR times (highest value)

The 16-byte AAD and 12-byte IV exist as four independently written copies that
must agree byte-for-byte:

| # | Location | Used for | Tested |
|---|---|---|---|
| 1 | `BlindsESP/main/include/FrameCrypto.h` | node, both directions | 14 tests, 10 mutations |
| 2 | `lora_client.cpp:106` `s_build_header_aad` / `:119` `s_derive_gcm_nonce` | hub **encrypt** path | indirectly |
| 3 | `lora_client.cpp:631` / `:643` — lambdas inside `set_response` | hub **decrypt** path | indirectly |
| 4 | `tests/proto_sim/sim/crypto.cpp:46,51` | the test oracle | it *is* the oracle |

Copies 2 and 3 are in the *same file*: the hub builds its AAD one way when
encrypting and another way when decrypting. They agree today — I compared them —
but nothing makes them agree.

**Why this is the top item.** Every mistake here produces one symptom,
`psa_aead_decrypt failed: -149`, which on this project has already stood for
three unrelated root causes. Worse, copy 4 is what the tests use to *build*
frames. If the sim and the hub drift together while the node stays correct, the
suite goes green and production fails — the exact "both ends wrong in the same
way" case the FrameCrypto tests were written to avoid.

**Fix.** `FrameCrypto.h` is deliberately dependency-free — no PSA, no ESP-IDF,
no FreeRTOS. The hub and the sim can both `#include` it. That collapses four
copies to one *and* transfers the existing mutation-verified coverage to the hub
for free. The include path is the only obstacle (the hub build would need to
reach into the BlindsESP tree, or the header gets vendored the way the protobuf
stubs are, with a drift gate — see §5).

Note one real asymmetry while they are separate: the node's `deriveIv()` refuses
a zero base nonce; the hub's `s_derive_gcm_nonce` only checks map membership.

---

## 2. `LORAListener::set_response` — CCN 61, 273 NLOC

The hub's mirror of the node's `onReceiveNew`, which was CCN 70 before being cut
to 15. Same shape, same causes:

- three crypto helpers defined as **lambdas inside the function** (lines 631–700);
- then REGISTER handling, address/registration filter, LOGIN, replay check,
  decrypt, post-decrypt session bookkeeping, and *two* dispatch ladders — one for
  decrypted inner messages (line ~992) and one for plaintext (line ~1017).

**This is the safest large refactor available**, for the same reason the node's
was: the harness compiles the *real* `lora_client.cpp` (`real_lora_client`, 33
tests drive it). Suggested split, mirroring what worked on the node:

```
decryptFrame()      – the decrypt path (or delete it entirely, see §1)
admitFrame()        – address filter, registration gate, replay window
dispatchDecrypted() – the inner-message ladder
dispatchPlaintext() – the plaintext ladder
handleRegister() / handleLogin() / handleBeacon()
```

Expected result ≈ CCN 15, with each handler in single digits.

The two dispatch ladders are near-duplicates (ACK / POSITION / BEACON in both).
They can likely collapse into one, but check first whether the plaintext path is
*deliberately* narrower — narrowing what a plaintext frame may do is a security
property, not an oversight.

---

## 3. Smaller complexity items

| Function | CCN | Note |
|---|---|---|
| `loracover/sensor/lora_sensor.cpp` `set_response` | 19 | a second dispatch ladder, same shape |
| `handle_beacon_` | 17 | beacon field fan-out; splitting per concern (clock / schedule / telemetry) would help readability more than CCN |
| `setup` | 15 | typical ESPHome setup; low value |
| `s_pack_operation_message` | 14 | packing switch; fine as-is |
| `lora_setSignalBandwidth` / `getSignalBandwidth` | 11 | vendored SX127x driver — leave, keep diffable |

Nothing here is urgent. Only `lora_sensor.cpp`'s `set_response` is worth doing,
and mainly for consistency with §2.

---

## 4. Dead code and cruft

Far less than the node had (698 lines were removed there). Commented-out code:
~30 lines in `lora_client.cpp`, ~33 in `lora_tracker.cpp`, ~33 in `lora.cpp`,
**~49 in `lora_sensor.cpp` — 20% of that file**. Only the last is worth a pass.

Six `TODO: Use unique address` comments in `lora_client.cpp` (lines 543, 578,
1248, 1252, 1770, 1776) all concern `senderaddress = kHubAddress`. Either the
hub genuinely has one address and the TODOs are stale — delete them — or there
is a real multi-hub question that should be written down properly once instead
of six times.

`lora.cpp` has `#ifdef TODO` (line 431) and a `FIXME: end hardware features`
(1333). It is a vendored SX127x driver; leave it alone.

---

## 5. Structural: the hub↔node contract has no single home

The protobuf stubs are now single-sourced with a regen script and two ctest
gates. The *crypto* contract has nothing equivalent — it is prose in comments
("mirrors decrypt_payload_gcm() in BlindsESP CmdDispatcher.cpp") plus four
hand-maintained copies.

The stub solution is the model: one source of truth, a script that writes every
consumer, and a test that fails when they diverge. Applying the same shape to
`FrameCrypto.h` would close §1 permanently rather than by care.

---

## Recommended order

1. **Single-source the wire format** (§1). Highest correctness value, and it
   hands the hub the node's existing mutation-verified tests.
2. **Split `set_response`** (§2). Largest complexity win, and well covered by 33
   existing tests, so it is verifiable the same way the node's was.
3. `lora_sensor.cpp` — its `set_response` (CCN 19) and its 49 commented lines.
4. Resolve or delete the six `TODO: Use unique address` comments.

Deliberately **not** recommended: restructuring `lora.cpp` or the bandwidth
helpers. Vendored driver code — the diffability against upstream is worth more
than the CCN.

---

## What I did not measure

- **Runtime/memory optimisation.** This is a complexity and duplication
  analysis. I have not profiled the hub, measured heap, or looked at airtime.
  If "optimisation" means performance rather than structure, that is a separate
  exercise needing different instruments.
- **`__init__.py` codegen** (630 lines in `lora_client`, 312 in `lora_tracker`).
  Python config validation; not examined.
- **`rika_stove`** (587 lines) — unrelated component, out of scope.
