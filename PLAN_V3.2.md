# Hypersion v3.2 plan — engine-derived improvements

## Context

RETUNE_PLAN.md was invalidated (Tier 3 v2 nodes=500000 SPSA produced 483 ELO TC swing). SPSA parameter tuning doesn't transfer across TCs at any node count Hypersion has tested. The remaining path forward is **more structural ports from reference engines**, not magnitude tuning.

## What worked this session (cumulative state for v3.1)

| Ship | Source engine | Bullet | LTC | Status |
|---|---|---|---|---|
| Tier 1 ContCorrHist | Berserk `history.h:79-85` | +90.6 | +20.9 | Verified both TCs |
| Tier 2 v2 ThreatSquareHistory (TC-gated) | RubiChess `search.cpp:128` | +96.2 (gate on) | ~0 expected (gate off) | TC-gating preserves bullet gain, no LTC regression |

**Key lesson**: TC-gating (bullet-conditional features) is the right pattern when a structural port helps bullet but hurts LTC. The `useThreatHist` flag in Worker (set in `iterative_deepen` from `tm.optimum()`) gates the feature cleanly.

## What didn't work (for v3.2 to avoid)

- Tier 3 SPSA at any node count: bullet-only, hurts LTC
- R1 multi-source corrhist extension: -3.5 ELO (Tier 1's blend is sufficient)
- R3 corrhist-LMR adjust: -206 ELO catastrophic (always)
- All single-feature SF18 history-cluster components: tombstoned

## New plan — engine-derived structural ports for v3.2

Three phases, each ~3-5 h implementation + ~1 h SPRT validation.

### Phase 1 — Multi-factor adaptive time management (Alexandria + Berserk hybrid)

**Motivation**: Hypersion has documented bullet flag-out weakness (issue #2 item 1). Berserk's 3-factor + Alexandria's multi-factor both address this. Time-mgmt changes are LESS NNUE-masked per chessprogramming.org research.

**Source**:
- Berserk: `search.c:332-352` — stabilityFactor + scoreChangeFactor + nodeCountFactor
- Alexandria: `time_manager.cpp:43-53` — 3 independent scaling curves

**Implementation**:
1. Track `bestMoveStability` across iterations (count of iterations where best move didn't change)
2. Track `scoreChangeMagnitude` across iterations (delta between consecutive scores)
3. Track `bestMoveNodesFraction` (fraction of total nodes spent on best move's subtree)
4. Compute `optScale = stabilityFactor × scoreChangeFactor × nodeCountFactor` per Berserk's formulas
5. Replace simple optimum-time with `optimum × optScale`

**SPRT plan**: 30g triage at TC 5+0.05 → 200g confirm → 200g LTC validation. If only bullet positive, TC-gate like Tier 2 v2.

**Expected ELO**: +5 to +20 at bullet (specifically targets bullet flag-out), 0 to +10 at LTC (better time use throughout the game).

### Phase 2 — Pawn history TC-gated (Obsidian-style port with bullet-only restriction)

**Motivation**: Obsidian's pawn-keyed history (`history.h:23`) gives ~part of their move-ordering advantage. Hypersion tested PawnHistory always-on at 4 weight settings, all tombstoned -22 to -95 ELO. The TC-gating pattern from Tier 2 v2 might salvage this for bullet.

**Source**: Obsidian `movepick.cpp::scoreQuiets` + `history.h:23` PawnHistory.

**Implementation**:
1. Add `PawnHistory` struct (4096 buckets × 16 pieces × 64 squares = 4 MB / thread)
2. Wire into `MovePicker::score_quiets` via TC-gated read (only if `usePawnHist` flag set)
3. Update at cutoff (similar to existing mainHist update)
4. Set `usePawnHist = useThreatHist` (reuse Tier 2 v2's gate)

**SPRT plan**: 30g triage → 200g confirm at bullet TC only. Don't test LTC if bullet positive (gate ensures LTC unaffected).

**Expected ELO**: -5 to +30 at bullet (was tombstoned always-on at -22; gate may unlock).

### Phase 3 — NNUE-aware singular extension margin (uses Tier 1 corrhist)

**Motivation**: Hypersion now has populated ContCorrHist tables from Tier 1. SF18 (`search.cpp:1142`) uses `correctionValue` to scale the SE margin: `int corrValAdj = std::abs(correctionValue) / 230673`. With Tier 1's data populated, this becomes viable. R3 (corrhist-LMR) failed catastrophically, but SE-margin is a different mechanism with smaller effect.

**Source**: SF18 `search.cpp:1142, 1574`.

**Implementation**:
1. Compute `correctionValue = staticEval - rawEval` (already done in current code)
2. In singular extension: `singularBeta -= std::abs(correctionValue) / DIVISOR;`
3. Calibrate divisor for Hypersion's cp-scale (~50-100 typical corrValue, want ~10-30 cp SE margin adjustment)

**SPRT plan**: 30g triage at TC 5+0.05. SE changes are touchy — abort if any negative signal.

**Expected ELO**: -10 to +15 (SE is fragile; could go either way).

## Execution order + stop-loss

1. Phase 1 (time mgmt) first — highest expected value, addresses documented weakness
2. Phase 2 (pawn HH TC-gated) second — leverages Tier 2 v2's gate infrastructure
3. Phase 3 (NNUE-aware SE) last — most fragile mechanism

**Stop-loss**: If 2 of 3 phases reject, halt and ship what's verified.

## Final deliverable

**v3.2** release with all verified ships:
- Tier 1 + Tier 2 v2 (from v3.1 baseline)
- Whatever passes from phases 1-3
- Final 6-engine tournament for measured cumulative gap closure

## Out of scope

- Further SPSA tuning (every approach tested has cross-TC issues)
- LowPlyHistory / 6-deep contHist (tombstoned with SF SPSA weights)
- Sparse L1 NNZ (memory-aggressive, low ROI vs implementation cost)
- NNUE network retraining (deferred per issue #2, needs human direction)

## Risk register

1. **Time mgmt + multi-factor**: highly TC-sensitive. May need TC-gating like Tier 2 v2.
2. **PawnHistory TC-gated**: gate threshold tuned for Tier 2 v2 may not be optimal here.
3. **SE margin**: any negative signal at 30g should abort immediately — SE is too easy to break.
4. **CPU time budget**: total ~12-15 hours including all SPRTs. Sleep the PC after.
