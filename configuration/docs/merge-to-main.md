# Getting to `main` before the bench session

> **DONE, 2026-09-21.** Both repos are adopted: hub `main` = `5241ce9`, node
> `main` = `0187123`, each an adoption commit whose tree is the development
> tree and whose second parent is the old `main`. Gates A (tree identical to
> what was tested), B (fast-forward) and C (927 tests) all passed before each
> push. The audit re-run against the preserved old `main` reports every path
> surviving.
>
> **Three things went differently from the plan below; the plan is left as
> written and the differences recorded here, because they are the parts a
> reader will hit.**
>
> 1. **The safety net is a BRANCH, not a tag.** This environment's GitHub
>    credential can create and update branches but cannot push tags (HTTP 403)
>    or delete branches. So the old `main` is preserved as the branch
>    **`main-before-adopt-2026-09`** in both repos. Same guarantee, different
>    ref type; recovery is
>    `git push --force-with-lease origin main-before-adopt-2026-09:main`.
> 2. **The legacy branches could not be deleted from here** — deletion is
>    refused by the same credential. They are harmless (see §5) but still
>    listed. To finish the job:
>    ```
>    git push origin --delete auto-mode-p0                               # both repos
>    git push origin --delete claude/blindsesp-battery-voltage-adc-78srik # both repos
>    ```
>    Do **not** delete `retired/battery-voltage-adc`: the ADC work was merged
>    by CONTENT, not by commit, so those commits are *not* in `main`'s history
>    and that branch is the only place they exist.
> 3. **The battery-voltage ADC branch was merged in first**, which was not in
>    the original plan and turned out to be the most valuable part of the whole
>    exercise. See §7.


**Goal:** one branch — `main`, in both repos — carrying the code the hardware
measurements will run against, with nothing lost and the currently-deployed
state recoverable by name.

**The short version:** this is not a merge. `main` shares **no common ancestor**
with the development line in either repo, so there is no three-way merge to
perform; there are two unrelated trees and a decision about which one is the
product. The development tree is the product. `main` is *adopted onto* it with a
merge commit that keeps `main`'s history reachable for provenance.

---

## 1. The topology, measured

```
HUB  github.com/smirre111/esphome_localcomponents

  main                          15 commits   root 6cad51b   tip 2026-07-27
  auto-mode-p0                  50 commits   root 871e54f   tip 2026-08-31
  claude/analysis-only-t4ter8   150 commits  root 871e54f   tip 2026-09-20

  merge-base(main, auto-mode-p0)              = NONE
  merge-base(main, claude/analysis-only-…)    = NONE
  merge-base(auto-mode-p0, claude/analysis-…) = 8b32c99  (= auto-mode-p0's TIP)

NODE github.com/smirre111/blindsesp

  main                          1 commit     root 3cd6a2d   tip 2026-07-27
  claude/analysis-only-t4ter8   52 commits   root 1fc730f   tip 2026-09-20

  merge-base(main, claude/analysis-…)         = NONE
```

Two things fall straight out of that:

**`auto-mode-p0` needs no work at all.** Its tip *is* the merge base with the
development branch, which means it is a direct ancestor — every commit on it is
already contained. Nothing to merge, nothing to lose. It only needs to stop
being a thing people wonder about (§5).

**`main` is a separate tree in both repos.** The node's is a single squashed
commit ("Node v1.0.12", 2026-07-27); the hub's is 15 commits from its own root.
`git merge` refuses unrelated histories outright, and with
`--allow-unrelated-histories` it would conflict on essentially every file
because no common version exists to diff against. A rebase is not available for
the same reason.

---

## 2. Why the development tree is the product, and not a judgement call

`main` is **older work, not different work.** The audit is scripted —
`tools/audit_main_adoption.sh`, run it yourself — and asks the only question
that matters for adoption: *does any path on `main` disappear unaccounted for?*

```
HUB   48 paths on main,  0 gone from the dev tree
NODE  91 paths on main,  3 gone — all three documented deletions
```

The three are `proto/blinds.pb-c.{c,h}` and `main/include/blinds.pb-c.h`:
redundant copies of the generated protobuf stubs. `proto/regen_stubs.sh`'s own
banner records that six copies across four locations existed, three were
redundant, and those three were deleted — leaving the two copies the two build
systems each genuinely need, kept in sync by the `schema_drift_*` ctest gates.

**The script deliberately does not diff content**, and that is the honest choice
rather than a shortcut: the development tree rewrote most of these files, so a
content diff reports thousands of replaced lines and cannot tell "replaced" from
"lost". Where the numbers looked alarming, each was checked by hand instead:

| Looked like a loss | What it actually is |
|---|---|
| `main/SystemCtrl.cpp` 62 KB → 24 KB | WiFi/OTA moved out into `main/OtaService.cpp` (plus `components/protocol_examples_common/`). Method-set comparison: `main` has 3 methods the dev tree lacks (`start_prj_wifi`, `get/setRegistered` — the state lives in `SystemCtrl.h` there); the dev tree has 14 `main` lacks, the whole auto-mode and schedule surface. |
| `loracover/sensor/lora_sensor.cpp` −129 lines | A block of commented-out BLE GATT dead code, deleted. |
| `main/utilities.cpp`, `mybutton.cpp`, `Buttons.cpp` shrank | Same shape: refactored or dead code removed. All three files still exist. |
| `blinds.pb-c.c` −464 / −1790 lines | Generated stubs, regenerated from a changed `.proto`. |
| `dependencies.lock` −79 lines | A lockfile. |

**`main`'s named features are all present in the development tree**, checked
individually: `battery_update_interval` (the node calls it `batteryInterval`,
default 900 s, read each cycle in `frtosTasks.cpp` so a config push takes
effect), the secrets.yaml credential move (**both** branches use `!secret` in
the same three places, and `secrets.yaml` is gitignored with
`secrets.yaml.example` tracked — so the deployed untracked file is undisturbed),
the cover-duration calibrations, slim on-air packets, and downlink encryption
gated on a confirmed session.

**This is evidence, not proof.** It was established by comparing trees and
spot-checking the alarming files, not by replaying 15 commits. If a bench result
contradicts something you believe `main` fixed, diff that specific file first
and do not assume either branch is complete.

---

## 3. The mechanism

One merge commit per repo whose **tree is exactly the development tree** and
whose **parents are (development tip, old main)**. `git merge -s ours` does
precisely this: strategy `ours` discards the other side's content while still
recording it as a parent. Note it is the *strategy* `-s ours`, not the option
`-X ours` — the latter only breaks ties and would try to merge content.

After it, old `main` is an ancestor of the new commit, so updating `main` is a
**fast-forward**: no force-push, and every commit that was ever on `main`
remains reachable.

### Run this per repo

```bash
# 0. A NAME for the current deployed state, so it is recoverable without reflog.
git fetch origin
git tag -a main-before-adopt-2026-09 origin/main \
        -m "main as deployed, before the development tree was adopted"
git push origin main-before-adopt-2026-09

# 1. Build the adoption commit ON A STAGING BRANCH — never on main directly.
git checkout -B adopt-main origin/claude/analysis-only-t4ter8
git merge -s ours --allow-unrelated-histories origin/main \
    -m "Adopt the development tree as main

main shared no common ancestor with this line, so this is an adoption
rather than a merge: the tree is this branch's, and main is recorded as a
second parent so its history stays reachable.

Nothing on main is lost — tools/audit_main_adoption.sh reports every path
surviving or documented. See docs/merge-to-main.md."

# 2. GATE A: the tree must be byte-identical to what was tested.
git diff --stat origin/claude/analysis-only-t4ter8 HEAD    # must print NOTHING

# 3. GATE B: old main must be an ancestor (so step 5 is a fast-forward).
git merge-base --is-ancestor origin/main HEAD && echo "fast-forward ok"

# 4. GATE C (hub only): the suite must be green on this exact commit.
#    927 tests, 0 failures.

# 5. Only with A, B and C passing:
git push origin HEAD:main
```

Gate A is the one that matters most. It is what makes "the code that was tested"
and "the code on `main`" provably the same object rather than two things that
ought to agree.

### Order

**Node first, then hub.** The node carries `proto/blinds.proto` and
`regen_stubs.sh` vendors the generated stubs and the shared headers into the
hub. If the two are ever momentarily inconsistent, better that the source of
truth moved first. Then, before the bench session, confirm the hub's vendored
copies match: the `schema_drift_*` ctest gates are exactly that check.

---

## 4. What this costs, stated plainly

- **Anyone holding a clone of `main` must re-clone or hard-reset.** `main` gains
  a second root line; its old commits remain reachable but the working tree
  changes completely. There is no way to avoid this and also have one branch.
- **It is not undone by a revert.** Recovery is
  `git push --force-with-lease origin main-before-adopt-2026-09:main`, which is
  why the tag in step 0 is not optional.
- **CI or anything pinned to a `main` commit SHA** will be looking at a tree
  that no longer resembles the branch.
- **Do not do this mid-session.** It is a five-minute job on a quiet repo and an
  afternoon on a busy one.

---

## 5. `auto-mode-p0`

Already fully contained in the development branch — its tip is the merge base.
After the adoption it is an ancestor of `main`, so it carries no unique work at
all. Either delete it or keep it as a tag; what it must not do is stay a branch
that looks like it might still hold something:

```bash
git tag -a auto-mode-p0-final origin/auto-mode-p0 \
        -m "auto-mode-p0 at its last commit; fully contained in main"
git push origin auto-mode-p0-final
git push origin --delete auto-mode-p0
```

---

## 6. Afterwards, before the bench

1. ~~Both repos on `main`, both green, `git status` clean.~~ **Done.**
2. ~~Re-run the audit against the preserved old `main`.~~ **Done** — clean in
   both repos (`tools/audit_main_adoption.sh origin/main-before-adopt-2026-09
   origin/main`).
3. ~~**Update `bench-runbook.md` §0a**~~ — **Done.** It said to use
   `claude/analysis-only-t4ter8` and explicitly not `main`, which after the
   adoption was not merely stale but backwards: a session following it would
   have tested a branch nobody maintains. It now points at `main`, explains why
   `git log` shows two unrelated root lines, and names
   `main-before-adopt-2026-09` for the previously deployed code.
4. **Remaining, and it needs a credential this session does not have:** delete
   `auto-mode-p0` and `claude/blindsesp-battery-voltage-adc-78srik` in both
   repos (commands in the banner at the top). Keep
   `retired/battery-voltage-adc`.
5. Then flash from `main` and start at the runbook's §0 preconditions.

**If the adoption is deferred**, change nothing: the runbook as written is
correct, and the bench session should run off
`claude/analysis-only-t4ter8`. Deferring costs nothing except the confusion of
two live trees. What is not safe is doing half of it.

---

## 7. The battery-ADC branch, and why merging it first mattered

Not in the original concept. It was raised as "would it make sense to merge this
as well, while the old main is still available", and the answer was yes for a
reason neither of us had in view: **the development tree had a real battery bug
and did not know it.**

`claude/blindsesp-battery-voltage-adc-78srik` exists in **both** repos, descends
from old `main`, and shares no ancestor with the development line. Its node half
fixes:

- **A ~11 % scale error.** The full scale was `3.95 / 2` V for a channel
  configured at 6 dB, whose true full scale is ~2.2 V. A 12.1 V pack read as
  ~10.7 V — already "empty" on the hub's 9.6–12.6 V 3S scale. **Every battery
  number a bench session took on the development tree would have come through
  that error.**
- **A panic.** `ESP_ERROR_CHECK` around the motor current-sense read reset the
  node mid-move whenever Wi-Fi held the ADC2 lock — provisioning or OTA, both of
  which a bench session uses.
- One unaveraged sample per measurement; unvalidated readings entering the
  last-known-good cache that *every* battery and position frame echoes; a cache
  starting at 0 V after each deep-sleep wake.

**How it was merged.** Three-way with old `main` as the base (`git merge-file`),
which is available precisely because the ADC branch descends from `main`. The two
ADC tasks were taken **wholesale** rather than hunk-merged: they are the debugged
implementation of a measurement this tree had wrong, and interleaving would have
produced a third version nobody has run. Everything else in `frtosTasks.cpp` is
the development tree's — the interrupt bodies and task loops from T-1. The two
sets of changes do not overlap, which is why this was tractable.

**The coupling that made "merge both halves together" load-bearing.** Read alone,
the hub branch's `UNIT_AMPERE` change was *wrong*: the development tree's node
filled `CoverPosition.current` with `getLastMotorCurrentAdcRaw()`, so declaring
amps would have labelled raw counts as amps — a wrong number wearing a confident
unit, and it is the current the endstop in `MotorPolicy.h` is judged against. A
note to that effect was written and then had to be reversed, because the **node**
repo's same-named branch is exactly the missing half: it converts on-node. Either
half alone ships the mislabelled number; both together are correct. That was only
visible with both repos' branches in view at once.

**It was verified by compiling**, which is only possible because T-1 got
`frtosTasks.cpp` under test first. The build found four things a flash would have
reported less clearly: a missing shim accessor, two absent ADC calibration
headers, a missing `ESP_ERR_NOT_SUPPORTED`, and two globals my splice had
duplicated. 927 tests pass.

**Still unverified on hardware**, and this belongs in the bench session: compare
the `Battery: raw=… -> …V` log line against a multimeter at the pack terminals
and adjust `kBattTrimFactor` in `main/frtosTasks.cpp` if the divider resistors
are off nominal.
