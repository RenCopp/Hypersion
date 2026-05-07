# Move ordering — MovePicker stages and history tables

## MovePicker state machine (src/movepick.cpp)

Order in which moves are returned by `next_move()`:

1. **TT move** — from TT entry's stored bestMove (if pseudo-legal).
2. **Captures (good)** — sorted by `7·victim_value + captureHistory`,
   only those with SEE ≥ 0 returned now. SEE-bad captures buffered.
3. **Killer 0** — most recent quiet causing cutoff at this ply.
4. **Killer 1** — second-most-recent.
5. **Quiets** — sorted by `mainHist + contHist1 + contHist2/2 + threat_bonus`.
6. **Captures (bad)** — SEE-negative captures buffered above.

Qsearch uses simpler picker: TT move → captures (sorted) → done.
Qsearch in check uses evasion picker: TT → all evasions sorted by capture-or-history.

## History tables (src/history.h)

| Table | Key | Magnitude |
|---|---|---|
| `ButterflyHistory` (mainHist) | [color][from][to] | -16384..+16384 |
| `CaptureHistory` (captureHist) | [piece][to][victim] | same |
| `KillerTable` (killers) | [ply] → 2 moves | n/a |
| `CounterMoveTable` | [piece][to] → counter move | n/a |
| `ContinuationHistory` (contHist[0,1]) | [piece][to] → [piece][to] table | -16384..+16384 |
| `CorrectionHistory` (pawnCorrHist, materialCorrHist) | [color][hash] → eval correction | small |

Updates use Stockfish-style soft-cap decay:
```cpp
entry += bonus - entry * abs(bonus) / HISTORY_MAX
```
This auto-decays large entries when bonus pushes against the cap.

## Threat-by-lesser bonus (in score_quiets)

For piece type `pt` (knight..queen), `threatByLesser[pt]` =  squares attacked
by enemy pieces strictly less valuable.
- Move TO a threatened square: penalty `-19 · PieceValueMG[pt]`.
- Move FROM a threatened square: bonus `+20 · PieceValueMG[pt]`.

Magnitude makes queen-escape worth ~50k (dominates other history). Source:
SF18 movepick.cpp::score(). Don't tune without 200g.

## Tombstoned move-ordering attempts

- **PawnHistory port** (SF18): -49 ELO @ 100g. Hypersion's pawnCorrHist
  fills similar role with different keying.
- **Low-ply history**: -70 ELO @ 30g. Possibly worth retry with paired
  parameter tuning.
- **Check-bonus +16384 after SEE ≥ -75 filter**: -11.6 ± 95.8 @ 30g.
  Within noise; threat-by-lesser already pulls forcing moves earlier.
- **4-ply continuation history**: -26 ELO. 2-ply seems to be the sweet spot.

## Why move ordering matters more than you think

Alpha-beta efficiency depends entirely on move ordering. With perfect
ordering: search depth d takes O(b^(d/2)) nodes (b = branching factor).
With random ordering: O(b^d). At d=20, b=35: 35^10 vs 35^20 = 10^15× more
nodes. So ANY improvement to move ordering compounds super-linearly.

This is why even "small" SF18 tweaks (threat bonus, history blending) are
large ELO wins. And why eval-side changes (which only narrow alpha-beta
windows) tend to give smaller absolute ELO than search/ordering changes.
