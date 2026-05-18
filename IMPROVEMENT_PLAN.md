# Hypersion improvement plan — 2026-05-18

Honest assessment after deep audit of 5 reference engines (Alexandria,
Berserk, Ethereal, Obsidian, RubiChess) + 6-engine tournament. Most of
the "obvious" ports are **already tested and tombstoned in Hypersion**.
Plan focuses only on the genuinely-unexplored candidates.

## Tournament-measured gap to close

| Engine | ELO vs Hyp | Notes |
|---|---|---|
| Stockfish (SF18) | +301 | Top reference, NN co-trained |
| Alexandria 9.0 | +301 | Hindsight reduction + multi-factor TM |
| RubiChess | +266 | Threat-square HH + material-hash corrhist |
| Obsidian | +266 | Threat-weighted quiets + Finny cache + pawn HH |
| Berserk | +266 | NNUE eval corrections (cont-hist blend) |

**Hypersion is -266 from field median.** Closing this gap is the goal.

---

## Already-tombstoned in Hypersion (DO NOT re-test)

Per `src/history.h:121-163`, `src/search.cpp:2273-2280, 2251-2272, 864`:

| Idea | Source | Hypersion result | Reason |
|---|---|---|---|
| **PawnHistory** (Obsidian/SF style) | Obsidian, SF18 | -22 to -95 ELO across 4 weight magnitudes | Disrupts already-tuned move ordering |
| **MaterialCorrHist (averaged blend)** | RubiChess-style | -26 ELO @ 200g | 14-bit material key has heavy collisions; averaging halved pawn-correction strength |
| **Hindsight reduction (priorReduction)** | Alexandria, SF18 | **-120 ELO @ 30g** | Anti-synergistic with cutoffCnt LMR; would need both retuned together |
| **LowPlyHistory** | SF18, Berserk | -70 ELO @ 30g | Phase 2/3/6 sweep failure |
| **Mate-threat extension** | SF18 | -11.6 ELO @ 30g | Within noise but trending negative |
| **SE double-extension** | SF18 | -107 ELO @ 30g | Search explosion |
| **CorrectionValue LMR adjust** | SF18 | -147 ELO @ 30g | Divisor miscalibrated |
| **Eval cache** | SF18, others | +11.6 ELO @ 30g → -35 @ 200g | i-cache pressure under conc=6 |
| **Joint SPSA (A+B cluster)** | Internal | -20.9 ELO @ 200g | Tombstoned in `spsa_joint_ab_out.json` |
| **Threat-by-lesser scoring** | Obsidian | **ALREADY IN HYPERSION** (`movepick.cpp:144-148`) | SPSA-tuned magnitudes via THREAT_BY_LESSER_* |

Hypersion is at a **tight local optimum**. Single-feature ports keep
regressing because bonuses, eval magnitudes, and pruning constants are
jointly tuned for the existing feature set.

---

## Genuinely unexplored candidates (the real plan)

### Tier 1 — Continuation-history correction (Berserk-style)

**Status**: Distinct from the rejected materialCorrHist (which used material-key averaging). Berserk's correction is keyed on **continuation pairs** (12×64×12×64 table) and applied **additively** with weight 17/8192. Hypersion's `materialCorrHist` tombstone explicitly says future attempts should try "additive with weight, or a separate small bonus" — Berserk's variant is exactly this.

**Source**: Berserk `history.h:79-85`, `types.h:203-204`
- Formula: `eval = rawEval + (31·pawnCorr + 17·cont1Corr + 46·cont2Corr) / 8192`
- New table: `contCorrection[12][64][12][64]` (per-thread, ~600 KB)

**Implementation steps**:
1. Add `ContCorrHist` struct in `src/history.h` (12×64×12×64 int16 entries)
2. Add `contCorrHist1` + `contCorrHist2` Worker members
3. Update at TT-write site: weighted update on (prev1, prev2) pair
4. Apply at eval read site: blend with pawnCorrHist using Berserk weights
5. Save/load + decay alongside pawnCorrHist

**Expected ELO**: +5 to +20 (Berserk-specific result; not guaranteed for Hypersion)
**Risk**: MEDIUM — adds ~1.2 MB / thread memory; could pressure L2/L3 cache
**SPRT cost**: 30g triage + 200g confirm + 200g LTC ≈ 30 min wall-clock

### Tier 2 — Threat-square keyed HH (RubiChess-style)

**Status**: Truly novel for Hypersion. Restructures the butterfly history from `[stm][move]` to `[stm][threatSq][from][to]`. RubiChess's threatSq is "the square that the opponent attacks which constrains our move".

**Source**: RubiChess `search.cpp:128`, `RubiChess.h:1693, 1837`
- Existing butterfly: `int16_t mainHist[2][2][64*64]` (16 KB) → New: `int16_t mainHist[2][64][2][64*64]` (1 MB)
- Need: `updateThreats<Color>()` early in search (already partially in `movepick.cpp::score_quiets`)
- Need: threatSquare derivation from threat bitboards

**Implementation steps**:
1. Verify Hypersion's threat bitboards (`movepick.cpp:126-148`) can be hoisted out of score_quiets into search()
2. Compute single threatSquare per ply (the most-threatened destination, or lsb of pinned-victim mask — choose RubiChess's exact derivation)
3. Refit `ButterflyHistory` indexing to `[color][threatSq][from][to]`
4. Verify update + lookup paths
5. Memory cost: 1 MB × NUM_THREADS

**Expected ELO**: 0 to +30 (high variance — RubiChess credits this for material part of their +266 over Hypersion)
**Risk**: HIGH — structural HH change; bonus magnitudes need re-tune
**SPRT cost**: 30g triage + 400g confirm + LTC ≈ 1.5 h wall-clock

### Tier 3 — Joint SPSA over coordinated parameter group

**Status**: All Hypersion failed-port tombstones cite "needs joint SPSA". The previous "A+B" joint SPSA was -20.9 ELO at 200g (search-only params). A different parameter group might break the local optimum.

**Candidate parameter group**:
- `THREAT_BY_LESSER_PENALTY` + `THREAT_BY_LESSER_BONUS` (existing)
- `HIST_BONUS_DEPTH2` + `HIST_BONUS_DEPTH1` + `HIST_BONUS_CAP`
- `CONT1_WEIGHT` + `CONT2_WEIGHT`
- `LMR_STATSCORE_DIV`

That's 8 parameters — too many for the typical 200-iter SPSA. Split into **2 sub-groups**:
- **Group A (move-ordering)**: THREAT_BY_LESSER × 2, HIST_BONUS × 3, CONT × 2 = 7 params
- **Group B (search)**: LMR_STATSCORE_DIV, NMP_EVAL_BETA_DIV, ASPIRATION_DELTA0 = 3 params (keep narrow)

**Implementation**:
1. Write `spsa_params_moveord.json` with the 7 move-ordering params
2. Run SPSA at TC-mode `--tc 5+0.05` (not nodes-mode — nodes-to-TC non-transfer documented in CLAUDE.md A4 + Hypersion's malus campaign)
3. ~500 iters × 4 games at TC = ~4-6 hours wall-clock
4. SPRT-validate the converged values vs current defaults at 200g

**Expected ELO**: -10 to +30 (joint SPSA is high-variance)
**Risk**: MEDIUM — Hypersion has documented track record of joint-SPSA campaigns rejecting at 200g (A+B was -20.9 ELO)
**SPRT cost**: 6+ hours wall-clock for SPSA + 200g validation

### Tier 4 — Finny cache extension (Obsidian-style)

**Status**: Hypersion has `g_finny` Finny cache (per audit). Obsidian uses 13 king buckets vs Hypersion's effective 8. Increasing buckets could reduce full-refresh frequency.

**Source**: Obsidian `nnue.h:81-82` (Finny 2×13 buckets), Hypersion `nnue.cpp` (existing finny logic)

**Implementation**:
1. Read Hypersion's Finny implementation
2. Compare bucket count + bucket-key derivation vs Obsidian
3. Extend if Hypersion is at 8 buckets and the perf wins are clear (need to measure refresh rate first)

**Expected ELO**: 0 to +10 (cache-friendliness improvement; not raw search strength)
**Risk**: MEDIUM — memory-aggressive (test at conc=2)
**SPRT cost**: 200g at conc=2 ≈ 1-2 h

---

## Execution plan

Strict serial — single-feature ports only. **Do NOT bundle.**

| Step | Action | Gate to next step |
|---|---|---|
| 1 | Tier 1: implement Berserk corrhist blend | 200g SPRT ≥ +10 ELO |
| 2 | Run cutechess tournament re-measurement | Hypersion ELO improves vs current binary |
| 3 | Tier 2: RubiChess threat-square HH | 200g SPRT ≥ +10 ELO |
| 4 | Tournament re-measurement | Confirm improvement |
| 5 | Tier 3: joint SPSA move-ordering group | Converged values 200g SPRT ≥ +10 ELO |
| 6 | Tier 4: Finny cache extension (if NPS bottleneck visible) | 200g at conc=2 SPRT ≥ +5 ELO |
| 7 | Final tournament + v3.1 release | — |

If a tier rejects: **tombstone in source with ELO ± CI**, do NOT proceed to next tier's bundle (the rejection signals the local optimum is tight). Skip to subsequent tier ONLY if there's a theory why the next tier's mechanism is independent.

## Verification checkpoints

Per CLAUDE.md Rules 1+2+3, every tier:
1. **Rule 2 — Read the source**: open the reference engine code, cite file:line, verify Hypersion doesn't already have it.
2. **Rule 3 — Web search**: if any heuristic is unusual, check chessprogramming wiki or talkchess for community context.
3. **Rule 1 — Cutechess SPRT**: triage (30g) → confirm (200g) → LTC (200g at 60+0.6) for ships, else tombstone.

## Realistic upside

Each tier is a +5 to +30 swing, but the historical pattern is mostly NEAR-NOISE results. A realistic outcome:
- 1 of 4 tiers ships clean (+10 to +20 ELO)
- 1 of 4 ships borderline (+5 to +10) and is tombstone-or-keep
- 2 of 4 reject

Cumulative expected: **+10 to +40 ELO net** (one solid + one borderline).

**This is not a path to closing the -266 gap by itself.** The realistic path to the field-median engines is:
- A **stronger NNUE network** specifically trained on Hypersion's search tree, OR
- A multi-month joint optimization campaign across 30+ params,
or both. The audit ports listed above are the search-quality contribution; the eval-quality contribution requires net retraining (out of scope per current "deferred — needs human direction" in issue #2).
