# Getting to `main` before the bench session

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

1. Both repos on `main`, both green, `git status` clean.
2. Re-run `tools/audit_main_adoption.sh main-before-adopt-2026-09 main` — it
   should now report the same clean result against the tag.
3. **Update `bench-runbook.md` §0a**: it currently tells the session to use
   `claude/analysis-only-t4ter8` and that `main` must not be used. Once this is
   done that instruction is not merely stale, it is backwards, and a bench
   session that follows it would test a branch nobody is maintaining any more.
4. Then flash from `main` and start at the runbook's §0 preconditions.

**If the adoption is deferred**, change nothing: the runbook as written is
correct, and the bench session should run off
`claude/analysis-only-t4ter8`. Deferring costs nothing except the confusion of
two live trees. What is not safe is doing half of it.
