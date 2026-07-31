# Hypersion deep audit — v3.6 improvement roadmap

## 2026-07-31 whole-tree pre-merge addendum

The entire tracked engine source was re-audited against Stockfish commit
`23cf5d827388e84d4389b40025ef401db4925f25` and Reckless commit
`d6603046e76d66edd43622ded23458da1af50c68`. The comparison covered position
state/move legality, UCI, search limits, SMP, TT, NNUE loading and incremental
evaluation, classical evaluation, move ordering, time management, Polyglot,
Syzygy/Fathom, build paths, and tests. The vendored Fathom copy was retained:
Reckless's `tbchess.c` is identical, while replacing Hypersion's remaining
files would discard local C++20, atomic, and aligned-I/O hardening.

Confirmed fixes from this pass:

- reject or normalize impossible and pinned en-passant targets and generate EP
  keys only when a legal capture exists;
- preserve a working NNUE when a replacement file is missing, truncated, or
  architecture-incompatible;
- enforce one global UCI node budget across all workers;
- honor the distance bound in `go mate N`;
- parse FEN/UCI counters and options without overflow or uninitialized values;
- replace undefined multidimensional history decay with recursive traversal;
- return the conventional one leaf for `perft 0`.

Verification completed on the final behavior: GCC release and Clang
ASan+UBSan builds; 12 release smoke checks (11 under sanitizers); 2,000
malformed FEN/UCI cases; 100-cycle
lifecycle stress; 2,000 release SMP searches at Threads=1/2/4/8; randomized
differential perft on 300 standard and 160 Chess960 positions (3,265,970 leaf
nodes); 500 incremental-vs-fresh NNUE comparisons; five identical Threads=1
bench runs at `1872788`; and a bounded 100-game CuteChess regression match
against the prior AVX-VNNI build (33-29-38, no crash, illegal move, disconnect,
or time forfeit). The match interval, +13.9 +/-54 Elo, is a regression sentinel
only and is not a strength claim.

Large telemetry collection and NNUE training remain explicitly out of scope.

**Date:** 2026-05-21 (post v3.5 ship)
**Current state:** HEAD `5adb326`, tag `v3.5`, +90 ELO bullet over pre-session HEAD
**Tournament gap:** −176 ELO vs Berserk/Obsidian/RubiChess (was −266 before this session — closed 34% in one session)
**WAC d8:** 191/198 = 96.5 % (classical eval saturated)

## Executive summary

The session that produced v3.5 confirmed that Hypersion's tournament gap is recoverable through **systematic scale-correction of cross-engine ports** (Rule 2.3). Of 7 attempted ports during this session, 4 shipped (+90 ELO combined), 3 confirmed non-scale failure modes. The +90 ELO came from low-effort literal-threshold corrections; the remaining gap requires **higher-effort structural changes** documented below.

Three categories ranked by ROI (ELO per dev hour):

| Category | Ceiling | Effort | Why now |
|---|---|---|---|
| **A. Structural search additions** | ~30–50 ELO | 4–8 hrs each | SF18 has features Hypersion lacks (doubleMargin SE, ttCapture flag with guard) |
| **B. NNUE retrain / sparsity** | ~20–40 ELO | 20–40 hrs | Hypersion uses SF18's net; own training would close the eval gap |
| **C. SPSA multi-parameter campaigns** | ~10–25 ELO | 12–24 hrs | Rules out single-parameter ceiling; coordinated tuning known to work |

**Recommended priority:** A first (cheap structural wins), then C (joint tuning on R52/R53/R54/R58 thresholds), then B (NNUE-side work).

---

## A. Structural search additions

### A1. SF18 doubleMargin / tripleMargin SE family (HIGH PRIORITY)

**What:** When a TT move is "singular" (other moves fail low in reduced search by margin M), SF18 extends by:
- 1 ply if M < doubleMargin
- 2 plies if M ≥ doubleMargin
- 3 plies if M ≥ tripleMargin (rare, ~1 % of SE firings)

**Hypersion:** Single +1 ply extension only. Tested depth++ extension at +70 (30g) → +10 (200g) → −5 (300g) — classic fakeout. **The fakeout happened because Hypersion lacks the doubleMargin/tripleMargin gating that controls how often the extra extension fires.**

**Implementation:**
1. Compute `correctionValue` in SE: how much TT score exceeded β
2. Add `doubleMargin = 11 + 174·ttPv` (SF18 src/search.cpp:1142)
3. Add `tripleMargin = 96 + 282·ttPv - 250·(ttScore > beta)` (SF18 src/search.cpp:1149)
4. Gate `extension` value on these margins
5. Apply Rule 2.3 scale correction (3.0× for SF → Hypersion margins)

**Effort:** 4–6 hours (struct edits + verify + SPRT 200g)
**Risk:** Medium (interaction with cutoffCnt + LMR)
**Expected ELO:** +15 to +30

### A2. SF18 ttCapture LMR adjustment with cutoffCnt guard

**What:** `if (ttCapture && m != ttMove) ++r` — when TT remembers a capture but current move isn't it, reduce more. SF18 src/search.cpp:1204-1205.

**Hypersion status:** TESTED. Result was −17.4 ± 36.1 ELO @ 200g (within noise band but consistently negative). Tombstone hypothesis: stacks with Hypersion's cutoffCnt adjustment (+1..+2 plies) to compound up to +3 ply LMR.

**Fix per Rule 2.4 Pattern F (reduction stacking):** Gate ttCapture on `(ss+1)->cutoffCnt <= 2`. The stacking only happens when cutoffCnt is high; gating prevents the over-prune.

**Implementation:**
```cpp
if (ttCapture && m != ttMove && (ss + 1)->cutoffCnt <= 2)
    ++r;
```

**Effort:** 0.5 hour (1-line guard + SPRT)
**Risk:** Low
**Expected ELO:** +5 to +15

### A3. SF18 priorReduction depth bump — REVERSE the sign

**What:** Hypersion previously tried `if ((ss-1)->reduction >= 3) ++depth` (under-search compensation). Rejected −120 ELO @ 30g.

**Per Rule 2.4 Pattern C (wrong sign):** Alexandria's hindsight reduction goes the OPPOSITE direction (`--depth`) and that shipped at +14 ELO (R53). The original SF18 port may have had the sign right but threshold wrong (per Pattern A).

**Hypothesis:** Try `if ((ss-1)->reduction >= 1 && (ss-1)->staticEval - ss->staticEval > 200) ++depth` — only bump depth when previous staticEval dropped sharply (genuine under-search signal).

**Effort:** 0.5 hour
**Risk:** Medium (was hard reject originally)
**Expected ELO:** +0 to +10 (low confidence)

### A4. RubiChess adaptive history extension threshold (full port)

**What:** Hypersion has **no history extension** feature. RubiChess extends 1 ply when both `contHist[ply-1][pieceTo]` and `contHist[ply-2][pieceTo]` exceed a threshold. The threshold self-adjusts in [9, 15] based on extension-to-quiet-move ratio.

**Hypersion gap:** Pattern B (feature absent), needs base implementation.

**Implementation:**
1. Add `he_threshold = 1000` (Hypersion-scaled) and `he_yes`/`he_all` counters to Worker struct
2. In move loop after computing newDepth, check `contHist1[pieceTo] > he_threshold && contHist2[pieceTo] > he_threshold` → extension = 1
3. Every 2²² checks, adjust threshold up or down based on extension ratio
4. Source: RubiChess src/search.cpp:795-822

**Effort:** 3–5 hours
**Risk:** Medium (interacts with existing singular extension)
**Expected ELO:** +5 to +15

### A5. Berserk NMP linear-R formula refactor

**What:** Hypersion's NMP is `R = 4 + d/4 + min(3, (eval-beta)/200)` (quadratic-ish via /4). Berserk uses integer-arithmetic linear: `R = 4 + 367d/1024 + min(9(e-β)/1024, 4)`.

**Hypersion status:** Recently shipped R58 which adjusted `NMP_EVAL_BETA_DIV` from 803 → 330 (scale-corrected). The structural shape is still quadratic-ish.

**Hypothesis:** Berserk's linear-R may fit Hypersion's tree better. Test as a refactor.

**Effort:** 2 hours
**Risk:** Medium-High (changes the heart of NMP)
**Expected ELO:** −10 to +15

---

## B. NNUE-side improvements

### B1. Sparse L1 NNUE SIMD (Berserk port)

**What:** Track non-zero L1 activations; skip zero-weight multiplications via NNZ (non-zero-zone) lookup. Berserk has AVX512/AVX2/NEON/scalar paths.

**Hypersion status:** Has L1-transform SIMD (+23.2 ELO shipped), but not sparse. Adding sparsity is incremental on top.

**Effort:** 15–25 hours (multi-path SIMD, AVX2/AVX512 conditional)
**Risk:** High (NNUE-internal, careful quantization)
**Expected ELO:** +6 to +10 NPS-driven

### B2. NNUE network retrain on Hypersion's eval signatures

**What:** Hypersion uses SF18's SFNNv10 network. Training a Hypersion-specific net on positions from Hypersion's own games + Hypersion's search dynamics would close the largest remaining eval gap.

**Effort:** 30–50 hours (data generation + training infrastructure + validation)
**Risk:** High (full retraining pipeline)
**Expected ELO:** +20 to +50

**Prerequisite:** A training corpus of 100M+ positions from Hypersion's recent games at fixed depth. Generation alone takes ~24 hours of compute on the user's hardware.

### B3. Obsidian-style 13 king buckets (architecture change)

**Hypersion has g_finny cache.** Number of buckets unverified but likely 8 (SF18 default). Increasing to 13 (Obsidian) would require:
1. Re-quantize the NNUE net for new bucket scheme — not portable without retraining
2. Update Finny cache layout

**Effort:** 20+ hours
**Risk:** Very High (NNUE-side changes without retrain almost always regress)
**Expected ELO:** +0 (without retrain) to +15 (with retrain)

**Verdict:** SKIP unless doing B2 first.

---

## C. SPSA multi-parameter campaigns

### C1. Joint tune of R52/R53/R54/R58 thresholds (HIGH PRIORITY)

**Why:** All four new ships have individual SPSA-untuned thresholds. Joint tuning could extract another +5 to +15 ELO.

**Parameters:**
- `R52_COMPLEXITY_THRESHOLD = 250` (range: 100 to 400)
- `R53_HINDSIGHT_THRESHOLD = 900` (range: 500 to 1500)
- `R54_RFP_FLOOR = 60` (range: 20 to 120)
- `R58_NMP_EVAL_BETA_DIV = 330` (range: 200 to 500)

**Campaign:** 12 g/iter, 1600 g total. ~6 hours @ TC 5+0.05 conc=2 (NNUE-heavy).

**Effort:** 6 hours (set up tune.h, run SPSA, validate sweep results, 200g confirm)
**Risk:** Low (each individual change already proven; joint tuning rarely regresses, often gains)
**Expected ELO:** +5 to +15

### C2. SPSA on history-update bonus formulas

**What:** Hypersion's history bonus formula is `min(2065, 30·d² + 16·d + 16)`. Berserk: `min(1708, 4d² + 191d − 118)`. Different shapes; SPSA can find Hypersion's optimum within both functional forms.

**Parameters:** 4–6 (BONUS_CAP, BONUS_DEPTH2_COEF, BONUS_DEPTH1_COEF, BONUS_OFFSET, MALUS_*, CONT2_WEIGHT)

**Effort:** 8 hours
**Risk:** Medium (Pattern H — local-optimum disruption observed in similar attempts)
**Expected ELO:** +0 to +10

### C3. LMR statScore divisor sweep

**What:** Hypersion's `r -= ss->statScore / 14000`. SF uses divisor 11248. Berserk uses different.

**Hypersion status:** Tested 11248 in the SPSA campaign — pooled neutral. Per Pattern H: Hypersion already at SPSA optimum at 14000.

**Verdict:** SKIP — already tested.

---

## D. Low-priority / parking lot

### D1. PawnHistory (4-weight tombstone)

Pattern H confirmed across 4 weights (1x, 2x, 0.5x, 0.25x). Would need joint SPSA over (butterfly weight, contHist weights, PawnHistory weight) — that's a 3-param SPSA, large campaign.

### D2. LowPlyHistory + 6-deep continuation history bundle

Pattern H confirmed in 4 SPRT runs. The tables produce +8.7 ELO raw signal but the 12-search-constant SPSA can't reach an optimum that beats Hypersion's defaults.

### D3. Alexandria multi-factor time management

6 coefficients require ~24h SPSA. Bullet-LTC ratio improvement uncertain.

### D4. Berserk LMP coefficients (`1.77 + 0.98·d²` etc.)

Different shape, would change move-count gates. Pattern H likely.

---

## Recommended next session plan

### Stage 1 (1 day, ~6 hours dev time): cheap structural wins
1. **A2 ttCapture+guard** (1-line patch, 0.5 hour) — fastest to test
2. **A1 doubleMargin SE** (4–6 hours including SPRT) — biggest single win

Expected outcome: +10 to +30 ELO additional.

### Stage 2 (1 day, ~8 hours): joint tuning
1. **C1 SPSA on R52/R53/R54/R58 thresholds** (6 hours) — extract residual ELO from this session's ships

Expected outcome: +5 to +15 ELO additional.

### Stage 3 (multi-day, conditional): NNUE work
1. Only if Stage 1+2 deliver as expected, consider **B1 sparse L1 SIMD** (15–25 hours)
2. **B2 NNUE retrain** is a 30+ hour commitment — defer unless gap remains > 100 ELO

### Reject for v3.6
- A3 priorReduction (low confidence, large previous regression)
- A5 NMP linear-R refactor (high risk, ambiguous gain)
- B3 Obsidian 13 king buckets (architecture-bound, needs retrain)
- C2 history bonus SPSA (Pattern H confirmed similar)
- D1-D4 parking lot (all Pattern H or B confirmed)

---

## Open questions

1. **Does the +90 ELO bullet hold up at LTC?** Current LTC measurement: +3.5 ± 35 ELO @ 200g. Borderline. A 400g LTC would tighten CI to ±25 and resolve whether the ratio is truly ~26:1 (concerning) or closer to 7:1 (good). Cost: ~3 hours machine time.

2. **Could the doubleMargin/tripleMargin port (A1) reverse the bullet/LTC ratio?** SE changes typically transfer well to LTC because they affect deep search trees. If A1 ships with +20 bullet and same +20 LTC, the ratio improvement would be substantial.

3. **Is the WAC ceiling of 191/198 actually saturated, or just at a local-optimum?** R56 showed a +205 ELO swing from scale-correction even though net ELO was 0 — there ARE more degrees of freedom in classical eval, just not in the directions we've tested. Worth one more sweep round focused on **interaction terms** (e.g., piece pair x phase) instead of additive scalars.

---

## Anti-recommendations (do NOT try)

1. **More single-magnitude eval-feature sweeps** — R39–R47 already exhausted single-feature directions. Pattern H is dominant.
2. **One-at-a-time SPSA on individual search params** — common-bugs.md anti-pattern "interior sweep point": 6 of 7 tested in v3.x cycle were 30g fakeouts.
3. **Anything touching the eval scale assumption** — Rule 2.3 derives from the 240/75 RFP ratio; changing RFP_MARGIN_PER_DEPTH would invalidate every scale-corrected port shipped this session.

---

## Source links

- v3.5 ship commits: `265fe39` (R53), `01cad41` (R52), `0de10d2` (R54), `d11997e` (R58)
- Tombstones with diagnostic value: `b825e31` (R39b Pattern B), `5adb326` (R55b Pattern H), `b01301f` (R57 Pattern H), `696126a` (R56 Pattern J)
- Standing rule additions: `c5d53e2` (Rule 2.3), `593d6c9` (Rule 2.4), `0dddfe2` (skill doc)
- This audit: `docs/AUDIT-v3.6.md`
