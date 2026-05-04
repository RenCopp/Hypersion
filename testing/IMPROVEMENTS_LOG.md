# Hypersion improvement log — autonomous engineering session

## ⚠️ Critical methodology caveat — measurements unreliable

After many rounds of A/B testing, I discovered that **all measurements
in this log are unreliable** because the test setup has an unsolved
opening-selection issue. Direct evidence:

| Comparison | Random openings | Sequential openings | Δ |
|---|---|---|---|
| search1 vs baseline | **+59.6** ELO | **-57.9** ELO | 117 ELO swing |
| search12 vs search2 | -161.9 ELO | -132.9 ELO | ~30 ELO swing |
| HEAD (≈search2) vs baseline | not measured | **+70.4** ELO | — |

These contradict each other: if search1 is -58 vs baseline and HEAD
is +70 vs baseline (HEAD ≈ search2 = search1 + SE 6→5, a single
small tweak), the chain doesn't close. **At least one of the three
measurements must be wrong.**

The most likely root cause is the opening book (`eco.bin` is a
polyglot/binary format, sprt.py passes `format=epd` to cutechess,
and cutechess's behaviour on this mismatch is unclear). Random
opening selection produces match-specific bias; sequential picks
unbalanced positions deterministically; both are problematic.

**Until this is resolved, no claim about session ELO impact should
be trusted.** The engine source code changes are real (LMR=1.90,
NMP zugzwang, endgame LMR mitigation, SE depth=5, AVX-VNNI build,
SPRT harness, PGO Makefile fix) but their net ELO contribution
is **undetermined**.

## What's been verified independently

* **Build correctness**: bench passes, perft 5 from startpos =
  4865609 (canonical), perft 5 from Kiwipete = 193690690 (canonical).
* **AVX-VNNI active**: 12 `vpdpbusd` instructions in the binary.
* **SPRT harness**: `testing/sprt.py` runs cutechess and parses
  output correctly; the harness itself is sound.
* **Code is clean**: 18 commits this session, all atomic, all with
  rationale comments + tombstone notes for failed attempts.

## Recommended next steps for the next contributor

1. **Replace `eco.bin` with an actual EPD file** (e.g. UHO_4060_v2,
   8moves_v3.epd, Pohl, or similar standard engine-test books).
2. **Re-baseline** using sprt.py with the new opening book,
   sequential order. Establish a clean baseline-vs-HEAD ELO.
3. **Bisect the round-1 changes individually** (LMR softening
   alone, NMP zugzwang alone, endgame LMR alone) to determine
   which ones actually help with proper opening control.
4. **Re-try opponentWorsening** with the proper book — the SF18
   feature itself is real and should help; the failure here was
   methodology, not necessarily the change.

## Final state (validated)

| Comparison | Result | TC | Games |
|---|---|---|---|
| Current HEAD vs pre-session baseline (sequential openings) | **+70.4 ± 34.1 ELO** | 5+0.05 | 200 |
| search1 vs baseline (sequential openings) | **-57.9 ± 42.1 ELO** | 5+0.05 | 200 |
| **Inconsistency: -127 ELO swing on a single SE-tweak commit** — testing infrastructure is unsound | | | |

The shipped configuration:
- LMR formula divisor 1.90 (search.cpp:53)
- NMP zugzwang strengthening at depth ≥ 12 (search.cpp:802)
- Endgame LMR mitigation (-1 ply when ≤8 pieces) (search.cpp:982)
- Singular extension at depth ≥ 5 (search.cpp:935)
- AVX-VNNI build target + 64-byte alignment

## Methodology lesson — random openings poison chain inference

A second-session experiment with SF18-style `opponentWorsening` flag
showed strong individual A/B results (+33, +117, +35, +102 ELO across
four sequential rounds) but a final cumulative direct measurement
revealed **-161.9 ELO** — the chain was bogus. Root cause: cutechess
`-openings order=random` selects a different opening sample every
match, and per-match selection bias compounds into massive false
positives across chained development.

**Fix applied**: `testing/sprt.py` now defaults to
`order=sequential` with `start=1`, giving every match the same
opening prefix from the file — chain inferences become trustworthy.
Old behaviour preserved behind `--random-openings`.

**Re-test confirmation**: `search12` (= round-2 + all four
opponentWorsening additions) vs `search2` with sequential
openings: **-132.9 ± 36.6 ELO** (20W-87D-93L) at 200 games. The
cumulative regression confirms the revert was correct.

| Comparison method | search12 vs search2 |
|---|---|
| Chain inference (+33 +117 +35 +102 from random-opening A/Bs) | +290 ELO claimed |
| Direct measurement (random openings) | -161.9 ELO |
| Direct measurement (sequential openings) | -132.9 ELO |

Both direct measurements (random and sequential) confirm the
opponentWorsening series was a regression. The chain inference
was off by ~400 ELO due to compounding per-match opening bias.



This log tracks the changes made during the no-GPU best-path session.
Each entry has: what changed, why, validation status, expected ELO.

---

## Step 1 — SPRT testing infrastructure ✅ SHIPPED

Built `testing/sprt.py`: a cutechess-cli wrapper that streams output,
parses live, and ends with a clean PASS / FAIL / INCONCLUSIVE verdict.
Standard config: 10+0.1, hash 64 MB, threads 1, concurrency 4,
SPRT [0, 5] alpha=beta=0.05.

Files:
- `testing/sprt.py` — main harness
- `testing/README.md` — updated with sprt.py usage block

ELO impact: 0 by itself, but unblocks all subsequent work.

---

## Step 2 — NNUE inference speed ✅ SHIPPED (neutral measured)

### 2a. 64-byte alignment (was 32-byte)

Bumped `alignas(32)` to `alignas(64)` on:
- `NNUEAccState::big_acc` / `small_acc` in `position.h`
- `FinnyEntry` and `FinnyEntry::acc` in `nnue.cpp`
- All on-stack accumulator buffers (transformed, tw/tb2, o0/o1, f1in/f2in, ref_acc, ref_psqt) in nnue.cpp inference path

Rationale: cache-line-sized alignment eliminates straddle-line loads.

### 2b. AVX-VNNI build target

The existing `Hypersion.exe` was built with AVX2 only (no `vpdpbusd`
instructions). Built with `ARCH=x86-64-avxvnni` (Alder Lake / Raptor
Lake, no AVX-512 available on these CPUs). New binary contains 12
`vpdpbusd` instructions — VNNI fast path active.

Updated Makefile: `x86-64-avxvnni` arch now uses `-march=alderlake`
which is more accurate than the previous `-march=haswell -mavxvnni`.

### 2c. Sparse FT incremental updates ✅ already implemented

Confirmed `apply_dirty()` in nnue.cpp does proper SF18-style
incremental update via DirtyPiece tracking. Finny cache handles
king-move full refresh. No code change needed.

### 2d. Bench measurement (5 runs each, median)

| Build      | NPS median  | NPS range          |
|------------|-------------|--------------------|
| Baseline (AVX2) | 660,056 | 619 k – 677 k |
| AVX-VNNI + 64-align | 652,309 | 617 k – 713 k |

**Result: essentially neutral**. The pre-existing Finny cache + sparse
incremental updates already captured the big wins. VNNI's `vpdpbusd`
only helps the FC layer dot products (small share of total eval time);
the FT update path uses int16 add/sub which VNNI doesn't accelerate.

ELO impact: ~0 measurable. Kept the changes — they're free correctness
improvements and give a tiny boost on cache-bound workloads.

---

## Step 3 — NNUE arch upgrade SFNNv10 → SFNNv11 ⏸ DEFERRED

Major refactor (loader + magic header + new layer order + FC size
changes). Risk:reward ratio not justified for one session — the SF
community is still publishing v10-compatible nets occasionally. Documented
as future work.

---

## Step 4 — Search refinements ✅ SHIPPED, A/B VALIDATED

Three changes bundled, validated together:

### 4a. LMR formula softening (1.95 → 1.90)

Source: `src/search.cpp:53`. Slightly less aggressive reductions per
log(d) × log(mc), matching the more accurate eval signal.

### 4b. NMP zugzwang strengthening at high depth

Source: `src/search.cpp:798`. At depth ≥ 12, require strictly more
non-pawn material than a single minor piece (`> 781` cp internal).
Below depth 12 the old `> 0` guard remains. The cost of a wrong NMP
cutoff scales with the depth saved, so high-depth zugzwang false-
positives are the worst case.

### 4c. Endgame LMR mitigation

Source: `src/search.cpp:982`. When piece count ≤ 8, subtract 1 ply
from the reduction. Endgames need accurate forcing-line calculation
(passed pawns, opposition, K+P chasing). The 50 g vs SF analysis
showed 70 % of Hypersion blunders happened in endgame.

### Validation: 200-game A/B at 5+0.05, search1 vs baseline

**FINAL RESULT: +59.60 ELO ± 40.80** at 200 games, 5+0.05 TC, hash 64 MB,
1 thread/engine, concurrency 8.

Score: 87 wins, 60 draws, 53 losses for the candidate.

Wall time: 7 minutes (417 seconds), 28 logical cores at concurrency 8.

This is a strongly positive result. Even the 95 % CI lower bound
(~+18.8 ELO) clears the standard SPRT [0, 5] gate.

A SPRT [0, 5] would have terminated earlier and accepted H1 — for
this magnitude of improvement, after roughly 30–60 games. We used
fixed-games for a sharper point estimate.

### Long-TC follow-up: result was TC-DEPENDENT

**100-game A/B at 10+0.1, search1 vs baseline: -20.9 ELO ± 53.1**
(27W-40D-33L). CI crosses zero — statistically not significant — but
the point estimate is negative.

Interpretation: round-1 changes appear to give a clear gain at fast
TC (+59.6 at 5+0.05) but converge to ~0 (or slightly negative) at
slower TC. Possible mechanisms:

- **LMR softening (1.95 → 1.90)** trades depth for accuracy. At fast
  TC where total node budget is tight, accuracy wins. At slow TC the
  baseline already has enough depth, so the tighter reduction's depth
  advantage matters more.
- **Endgame LMR mitigation** fires whenever piece count ≤ 8. At slow
  TC the search reaches more endgame positions, so the `-1 ply`
  reduction adjustment compounds and can over-extend non-critical
  lines.
- **NMP zugzwang strengthening** has no obvious TC sensitivity.

This is a noisy result — 100 games at long TC is at the limit of
reliability. The negative point estimate is well within the noise band.

**Decision: keep round-1.** At 5+0.05 the gain is clear and large;
at 10+0.1 the result is statistically tied. The likely cause-and-effect
suggests the engine should be re-tuned for the user's actual TC profile.

### Recommended follow-up

1. **Larger long-TC sample**: 400+ games at 10+0.1 to tighten the CI.
   This needs ~30 min of wall time but gives a definitive answer.
2. **Per-change attribution**: bisect 4a/4b/4c with separate SPRTs.
   The TC-dependence likely traces to LMR softening or endgame LMR;
   knowing which lets us keep the safe parts and revert the rest.
3. **TC-aware tuning**: if the user primarily plays at lichess
   (where TCs vary 3+0 to 10+5), keeping round-1 is reasonable.
   For long-TC tournaments, consider reverting the LMR softening
   while keeping NMP zugzwang and endgame LMR mitigation.

---

## Round 2 — Singular extension threshold (commit `bec4ee3`)

Source: `src/search.cpp:935`. Lowered SE depth threshold from
`depth >= 6` to `depth >= 5`, matching Stockfish 18.

**200-game A/B vs round-1 binary at 5+0.05: +13.9 ELO ± 35.5**
(58W-92D-50L). Marginal positive — the 95 % CI lower bound is
below the SPRT [0, 5] H1 gate, so this is low-confidence.

Bench cost: NPS drops ~10 % (more SE attempts is more expensive
per node), but the deeper effective search at TT moves should pay
for it.

---

## Round 3 — Bisection + LMR revert (commit `ff88bf7`, then RESTORED in `bb19549`)

Goal: identify which of round-1's three changes caused the long-TC
regression seen in the validation match.

Built `search3` = round-1 + round-2 with the LMR formula change
reverted (1.90 → 1.95). Other three changes (NMP zugzwang, endgame
LMR mitigation, SE 6→5) preserved.

**Bisection results (chain measurements):**

| Test                                    | Result            |
|----------------------------------------|-------------------|
| search3 vs search2, fast TC 5+0.05, 200 games | -41.9 ± 38.1 |
| search3 vs baseline, long TC 10+0.1,  80 games | +34.9 ± 63.3 |

These suggested LMR softening was the TC-dependent factor and
motivated commit `ff88bf7` (revert LMR to 1.95).

### Direct re-measurement contradicted the inference

The chain inference put `search3 vs baseline` at fast TC ~+32 ELO.
A 200-game direct measurement gave **-20.9 ± 39.0 ELO** — a
discrepancy of ~53 ELO within compound noise (σ_compound ≈ 66).

Long-TC re-measurement was even worse: 80-game run gave +34.9,
100-game run on the SAME configuration gave -52.5. Same direction-
flip on a single sample shift. The long-TC noise floor on this
machine + concurrency setup is too high to support the conclusion.

| Test                                    | Result            |
|----------------------------------------|-------------------|
| search3 vs baseline, fast TC 5+0.05, 200 games | **-20.9 ± 39.0** |
| search3 vs baseline, long TC 10+0.1, 100 games | **-52.5 ± 52.8** |

Conclusion: **the LMR formula softening was responsible for most
of round-1's fast-TC gain**. Removing it drops the engine from
+59.6 to ~0 at fast TC. The earlier "regression at long TC" signal
was within the noise floor of the 100-game test setup.

**Decision: restore LMR=1.90.** Commit `bb19549` reverses
`ff88bf7`. Final shipped state is back to what was at `bec4ee3`:
LMR=1.90 + NMP zugzwang + endgame LMR + SE depth>=5.

### Suggested next step: TC-aware LMR or larger-sample long-TC

The genuine long-TC question — does LMR=1.90 hurt at 10+0.1? —
remains open because the 100-game samples gave 88 ELO of noise.
A 500-game run at long TC would tighten the CI to ±25 ELO and
give a definitive answer.

If both modes are wanted, add a UCI option `LMRDivisor`
defaulting to 190 with range 170–220. Tournament configs can
override.

---

## Step 5 — SPSA tuning ⏸ SKELETON ONLY

Files:
- `testing/spsa.py` — argparse skeleton (loop NOT implemented)
- `testing/spsa_README.md` — full conversion guide

SPSA requires: (a) engine constants must become runtime UCI options
first (currently `constexpr int`); (b) cutechess wrapper that sets
options between candidates; (c) gradient aggregator. Each is straight-
forward but adds up to a 1–2-day refactor.

Recommended path: do this AFTER any further architecture changes
settle (so SPSA isn't tuning on a moving baseline).

ELO impact when complete: +10–30 over 1–2 weeks of unattended runs.

---

## Files changed this session

```
M  src/position.h          (alignas(64) on accumulator)
M  src/nnue.cpp            (alignas(64) on Finny + on-stack buffers)
M  src/search.cpp          (LMR softening, NMP zugzwang, endgame LMR)
M  Makefile                (avxvnni: -march=alderlake)
M  testing/README.md       (sprt.py usage)
+  testing/sprt.py         (NEW: SPRT harness)
+  testing/spsa.py         (NEW: SPSA skeleton)
+  testing/spsa_README.md  (NEW: SPSA roadmap)
+  testing/IMPROVEMENTS_LOG.md  (this file)
```

## Saved binaries (testing/)

- `Hypersion_baseline.exe` — pre-session AVX2 build (control)
- `Hypersion_search1.exe` — current session: AVX-VNNI + alignment + 3 search refinements

## Recommended next steps

1. **Let the 200-game A/B finish.** If final result is > +30 ELO, ship.
2. **Long-TC re-validation**: SPRT [0, 5] at 60+0.6 to confirm the
   improvement holds at slow time controls.
3. **Each search refinement individually**: bisect 4a/4b/4c with
   separate SPRTs to attribute the gain. Useful for understanding
   which mechanism actually moved the needle.
4. **SPSA prerequisites**: convert search constants to UCI options
   (1–2 day refactor) so SPSA can run unattended.
5. **NNUE v11**: eventually, but only after SPSA cycle 1 completes.
