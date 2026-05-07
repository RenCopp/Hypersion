# Search heuristics — Hypersion vs SF18

## Iterative deepening + aspiration windows

- Standard IID. Aspiration window starts at `delta=10` cp (in 5x scale).
- Failed bound widens window: `delta = max(delta * 2, 25)`.
- After delta ≥ 1000, fully open: `[-INF, +INF]`.
- Score stability tracked: `stableIters++` if same bestmove + small swing.

## Move-count pruning (LMP)

Quiet moves beyond `improving_quiet_count(d, improving)` get pruned at
shallow depths. Hypersion uses SF-style depth-based formula. Tuned
empirically; don't change without paired re-tuning of LMR.

## Late-move reductions (LMR)

`Reductions[d][mc] = log(d) * log(mc) / 1.85` (Hypersion v2.0).
- 1.85 vs 1.90 was tested at +3.5 ± 36.8 ELO @ 200g (within noise).
- **Don't tune the divisor without re-running 200g** — small changes here
  swing wildly at 30g.

Per-move LMR adjustments:
- Cut node: +1 ply reduction
- TT move singular: -1 ply (singular extension implicit)
- TT move improving: -1 ply
- Capture: -1 ply
- Killer: -1 ply
- History: scaled bonus/penalty (statScore / 14000 in SF, similar magnitude in Hypersion)

## Null-move pruning (NMP)

Hypersion: `R = 4 + d/4 + min(3, (eval-beta)/200)`.
- nmpR5 (R=5 base) tested at +107 → +0 ELO @ 200g (rejected).
- Material guard: skip NMP if non-pawn material < threshold (zugzwang).
- **Verification search at depth ≥ 16** tested at -20.9 ± 38.3 ELO (rejected,
  Hypersion's material guard already covers the cases SF's verification does).

## Singular extensions (SE)

Standard SF-style: at TT-stored move at depth ≥ 6, do reduced-depth search
of all OTHER moves. If they fail low by margin, the TT move is "singular"
→ extend by 1 ply.
- **Double extension** (extend by 2 if very singular) tested at -X ELO
  (rejected). See search.cpp tombstone.

## Razoring

Static eval ≤ alpha - razoring_margin(d) → drop to qsearch.
- razPD480 (depth+480 cp formula) tested at +47 @ 30g → -52 @ 61g (aborted).
- Current formula is search.cpp::razor_margin(d).

## Reverse futility (RFP / static null pruning)

Static eval ≥ beta + RFP_margin(d, improving) → return eval.
- Tightening this is Hypersion-specific because eval is at 5x scale.

## Probcut

Skip moves that fail high in shallow search with `probcutBeta = beta + margin`.
Can use SE-style reduced search to verify. Hypersion has it; magnitudes
documented in search.cpp.

## Continuation history (contHist)

- contHist[0]: 1-ply lookback (counter-move history)
- contHist[1]: 2-ply lookback (read at half weight in score_quiets)
- **4-ply lookback with decaying weights** tested at -26 ELO (rejected).

## History-based pruning

- Quiet moves with statScore < threshold(d) skipped at shallow depths.
- statScore = mainHist + contHist1 + contHist2/2.

## Time management (src/timeman.cpp + search.cpp:686-797)

Soft-stop scale factors (multiplicative):
- stableIters ≥ 4 → 0.5 (stable, wrap up early)
- stableIters ≥ 2 → 0.75
- bestMoveChanges ≥ 3 at d ≤ 12 → ×1.4 (volatile, spend more)
- fallingEval (`1 + drop/1000` clamped [0.6, 1.7])
- pieceCount ≤ 8 → ×1.6 (endgame bonus)
- pieceCount ≤ 12 → ×1.4
- pieceCount ≤ 16 → ×1.2
- Easy-move (gap ≥ 150 + stable) → cap at 0.4
- Best-move-effort ≥ 93.34% → ×0.76

**KNOWN BUG**: bullet flag-out with passed pawn. Two attempted fixes
(timeman mtg compression, scramble-time bonus) regressed in 200g matches.
Mitigation: configure Syzygy.

## Constants by 5x-scale

Hypersion eval is internally at SF's 5×. So when porting SF constants:
- SF margin 100 cp → Hypersion 500
- SF threshold 75 cp → Hypersion 375
- LMR bonus/penalty divisors: keep same (history magnitudes same)
- HistoryStat::MAX: same

This 5× scaling is the single most common source of port regressions.
Always check before assuming "SF18 says X cp, port directly".
