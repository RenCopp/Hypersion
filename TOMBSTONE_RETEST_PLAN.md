# Tombstone retest plan — 2026-05-18

After Tier 1's massive +90.6 ELO SHIP (`aca639a`, Berserk continuation-
correction blend), the eval-landscape has fundamentally changed.
Tombstones measured **before** ContCorrHist shipped may now behave
differently. This plan systematically retests them.

## Methodology premise (web research, see prior turn)

**NNUE-search coupling is documented** (Stockfish issues #2981, #3365,
#4678): test patches under the network you ship. Don't switch to
classical-only. Most ELO-masking happens to:
- Static-eval pruning (RFP, NMP, futility) — **strongly masked**
- Eval-correction histories — **directly substitutable by NNUE**
- Move ordering, LMR move-count gates, time mgmt — **less masked**

**Implication**: tombstones in NNUE-strongly-masked categories
(RFP/NMP/futility sweeps) won't suddenly unmask. Tombstones in
move-ordering and time-management categories are more retest-worthy.
Eval-correction tombstones become especially retestable now that
ContCorrHist exists and provides a new signal source.

## Tombstone inventory (collected from source + git log)

### Category A — eval-correction-related (HIGH retest priority)

| Tombstone | Original result | Why retest now |
|---|---|---|
| **A1: MaterialCorrHist additive blend** | -26 ELO @ 200g (averaged blend) | Tombstone explicitly says "try additive with weight". Tier 1 just proved additive blend works (+90 ELO). Material-hash bucket signal may now combine usefully with pawn + cont1 + cont2. |
| **A2: minorCorrHist + nonPawnCorrHist[2]** | -34.9 ELO @ LTC 20g (bundled) | Originally bundled with other SF18 ports. Solo retest with the 3 sources tier 1 added might gel. |
| **A3: CorrectionValue LMR adjust** | -147 ELO @ 30g (miscalibrated divisor) | Was catastrophic only because divisor was wrong (corr stored cp*256). With Tier 1's well-bounded ContCorrHist and known cp magnitudes, the divisor can be calibrated correctly. |

### Category B — move-ordering-related (MEDIUM retest priority)

| Tombstone | Original result | Why retest |
|---|---|---|
| **B1: PawnHistory (Obsidian/SF style)** | -22 to -95 ELO across 4 weight settings | All prior tests were SINGLE-feature. Per source comment: needs joint SPSA over (bonus magnitudes, butterfly weight, contHist weights). |
| **B2: LowPlyHistory (SF18)** | -70.4 ELO @ 30g | Same root cause as PawnHistory. Solo failure. Joint test with PawnHistory + 6-deep contHist may activate the cluster SF18 has (search.cpp:2956 explicit note). |
| **B3: 6-deep continuation history** | -24 ELO solo (referenced in search.cpp tombstones) | Same — needs the LowPlyHistory + PawnHistory companions per SF's cluster design. |
| **B4: stable_sort movepicker** | -56.1 ELO solo | Likely measured before recent movepick.cpp threat-by-lesser additions. Movepicker mechanics changed. Worth a fresh re-measurement. |
| **B5: Threat-by-lesser check bonus** | -20.9 ELO @ 100g | Original signal was in noise band. Worth retest, but low-priority. |

### Category C — search-heuristic (NNUE-masked, LOW retest priority)

| Tombstone | Original result | Verdict |
|---|---|---|
| **C1: Joint A+B SPSA** | -20.9 ELO @ 200g (nodes-tuned values) | TC-mode SPSA over a *different* parameter group might work. Don't re-run the same group. |
| **C2: NMP verification (depth ≥ 16)** | -20.9 ELO @ 200g | NMP-related; NNUE-masked per research. Skip. |
| **C3: SE double-extension** | -107.5 ELO @ 30g (search explosion) | Too aggressive; not retestable without rewriting the gate. |
| **C4: Mate-threat extension** | -11.6 ELO @ 30g (noise band) | Marginal; might pair with SE refinement. Low priority. |
| **C5: SPSA over malus split** | -19.1 ELO @ 200g (this session) | Already exhaustively SPSA'd. Skip. |
| **C6: SF18 futility +85/+161 bonuses** | -34.9 ELO @ 30g | NNUE-masked futility per research. Skip. |
| **C7: search #16 rule50≥96 gate** | -3.5 ELO @ 200g (this session) | Correctness fix only; never positive. Skip. |

### Category D — NNUE / eval infrastructure (HIGH retest, if joint)

| Tombstone | Original result | Retest plan |
|---|---|---|
| **D1: SMALL_FALLBACK_TH = 150** | -82.6 ELO @ 96.8 CI | Was a single-knob change. With current ContCorrHist providing better eval signal, the small-net might be useful in different positions. Joint with PSQT-material threshold tune. |
| **D2: SF18 scrambled FC weight layout** | tombstoned | Pure performance — unrelated to ContCorrHist. Skip unless NPS optimization later. |
| **D3: NNUE Rule-50 damping** | -13.9 ELO @ 200g | Documented SF feature. Retest after ContCorrHist since both affect eval. |

### Category E — time management (Hypersion has known weakness)

| Tombstone | Original result | Retest |
|---|---|---|
| **E1: A4 SPSA on time-mgmt scales** | -3.5 ELO @ 200g (within noise) | Could retry with TC-mode SPSA over fewer params (3 vs 6). Low confidence. |
| **E2: Bullet flag-out fix attempts** | Several rejected. See timeman.cpp:99-147 | Hypersion issue #2 item 1. Joint test with Berserk-style 3-factor TM as fresh approach. |
| **E3: Empty-TT boost (T1.1 Kirin V8 port)** | tombstoned | Move counter exists (ownSearchIndex). Could retry with different multiplier. |

---

## Joint feature combos (where source comments explicitly suggest)

Tombstones in source explicitly suggest these joint tests:

### Combo 1: "SF18 history infrastructure cluster" (from `search.cpp:2956`)
```
- LowPlyHistory (separate table for ply < 4)
- PawnHistory (Obsidian/SF style)
- 6-deep contHist (vs Hypersion's 2-deep)
- LMR statScore divisor recalibrated for the wider history sum
```
**Expected ELO**: SF gets a substantial chunk of its move-ordering ELO from this cluster. If Hypersion can replicate it, +20 to +40 ELO range.
**Cost**: 4 features ported together + joint SPSA over scaling.

### Combo 2: "Multi-source corrhist" (extending Tier 1)
```
- pawnCorrHist (existing)
- contCorrHist1 + contCorrHist2 (Tier 1 SHIPPED)
- materialCorrHist as additive (Category A1 retest)
- minorCorrHist + nonPawnCorrHist (Category A2 retest)
```
Hypersion would then have 6 correction sources matching SF18's setup.
**Expected ELO**: +5 to +20 incremental over Tier 1's +90.

### Combo 3: "Time management modernization"
```
- Berserk-style 3-factor TM (stability + score-change + node-distribution)
- Bullet flag-out fix with adaptive overhead
- Stable-iter time scaling (currently A11)
```
**Expected ELO**: +5 to +15 at bullet TC (where Hypersion is weakest);
neutral at LTC.

---

## Recommended execution order

After the current Tier 2-4 plan (RubiChess HH, joint SPSA, Finny cache),
add these tombstone retest tiers:

### Tier R1 — A1+A2 multi-source corrhist (HIGHEST CONFIDENCE)

**Why first**: Tier 1's +90 ELO proves the additive-corrhist mechanism
works. Extending with materialCorrHist + minorCorrHist + nonPawnCorrHist
as additive (NOT averaged) is the cheapest extension.
**Implementation**: same struct pattern as ContCorrHist, ~3-5 new tables.
**Memory**: ~5-8 MB extra per thread total.
**SPRT cost**: 30g + 200g + LTC ≈ 1.5 h wall-clock.

### Tier R2 — Combo 1: SF18 history infrastructure cluster

**Why next**: Most likely to break the local optimum that single-feature
ports keep failing to escape. Requires joint SPSA after the ports land.
**Implementation**: 4 features + 5-8 SPSA parameters.
**SPRT cost**: 200g implementation test + 4-8 hour SPSA + 200g + LTC ≈ 12 h
wall-clock for the full cycle.

### Tier R3 — A3 CorrectionValue LMR adjust (with proper divisor)

**Why**: Was -147 ELO purely due to divisor calibration. Now we have
ContCorrHist data with known cp magnitudes — can compute proper divisor
from data range. Cheap to retry.
**SPRT cost**: 30g + 200g ≈ 30 min.

### Tier R4 — B4 stable_sort movepicker

**Why**: Old tombstone (-56 ELO). Movepicker mechanics changed (Tier 3+4
audit added GOOD/BAD_QUIET stages, threat-by-lesser scoring). Worth a
fresh re-measurement to validate or re-tombstone.
**SPRT cost**: 30g + 200g ≈ 30 min.

### Tier R5 — D3 NNUE rule-50 damping

**Why**: Standard SF feature. -13.9 ELO previous measurement preceded
multiple eval-pipeline audit fixes. Worth re-measurement now.
**SPRT cost**: 30g + 200g ≈ 30 min.

### Tier R6 — Combo 3: time management modernization

**Why**: Hypersion's documented bullet weakness (issue #2 item 1).
Different approach: 3-factor TM as new structure, not a parameter tune.
**SPRT cost**: 200g at bullet TC + 200g LTC + bullet-flagout regression
test ≈ 2 h.

### Skip / defer

- All NNUE-masked categories (C1 except joint, C2, C3, C5, C6, C7)
- D2 NNUE weight layout (pure perf, NPS bench needed not SPRT)
- Anything that's been re-tombstoned this session

---

## Verification before each retest

Per CLAUDE.md Rules:
1. **Rule 1**: cutechess SPRT via testing/sprt.py
2. **Rule 2**: re-read source engines if porting (Berserk for corrhist family,
   Alexandria for time mgmt) AND read the existing Hypersion tombstone
3. **Rule 3**: web search if a tombstone's reason is unclear

For tombstone retests specifically:
- Read the ORIGINAL tombstone comment in source (cite line)
- Identify what infrastructure has changed since the original test
- State the hypothesis for why the retest might land differently
- If the hypothesis is "we're hopeful" without a concrete change, SKIP

## Realistic expected outcome

- **Tier R1**: high confidence, +10 to +30 ELO incremental
- **Tier R2**: moderate confidence, 0 to +30 ELO (joint SPSA variance)
- **Tier R3-R5**: each ~50/50 to ship, +0 to +10 ELO if positive
- **Tier R6**: low confidence, may need NN retrain to actually work

**Total expected from retest campaign**: +15 to +50 ELO additive on top
of the +90 Tier 1 already shipped + whatever Tiers 2-4 deliver.

**This is still not enough to close the -266 gap to field-median engines.**
But the cumulative path is real:
- Tier 1 ship: +90 ELO measured
- Tiers 2-4 expected: +10 to +40
- Retests R1-R6 expected: +15 to +50
- **Cumulative best case: +155 ELO** → Hypersion v3.1 (~v3.0 + 145 net)
  could be roughly tied with field-median (Berserk/Obsidian/RubiChess).
- **Cumulative worst case: +90 ELO** → Hypersion v3.1 still ~-176 from
  field median, but a clear improvement over v3.0.
