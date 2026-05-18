# RubiChess — reference notes for Hypersion development

## What it is
Modern C++ UCI engine by Andreas Matthies. ~3650-3700 ELO (CCRL). Long-running, one of the strongest non-Stockfish-derived engines. Supports dual NNUE architectures (HalfKP v1 + HalfKAv2_hm v5).

## Source location
`C:\Engine\Engines\RubiChess-master\RubiChess-master\` — extracted. Prebuilt avx2 at `C:\Engine\Engines\prebuilt\rubichess\RubiChess.exe` (with `nn-bc638d5ec9-20240730.nnue` 20 MB alongside).

## Distinctive search heuristics

| Heuristic | Detail | File:line |
|---|---|---|
| **LMR formula (improving split)** | `log(d · lmrlogf0/100) · log(m) · lmrf0/100` for non-improving;  `log(d · lmrlogf1/100) · log(m·2) · lmrf1/100` for improving — **multiplicative on both axes** | search.cpp:45-47 |
| **Threat pruning (Koivisto-style)** | If `!check && depth==1 && staticeval > β+margin && !threats` → return β | search.cpp:580-582 |
| **NMP eval-ratio** | `R = 3 + d/nmmreddepthratio − eval/nmmredevalratio` with PV factor | params:2352-2356 |
| **Razoring** | `staticeval < ralpha − margin − d·razordepthfactor` (depth-scaled margin) | search.cpp:560-561 |
| **RFP improving-aware** | `staticeval − d·(futilityreversedepthfactor − futilityreverseimproved·improved) > β` | search.cpp:591 |
| **Singular extension** | Double-extension guard via `extensionguard`; margin = `max(hashscore − singularmarginperdepth·d, SCOREBLACKWINS)` | search.cpp:749-770 |
| **History extension (adaptive)** | Threshold dynamically adjusted in [9, 15] range based on extension usage stats | search.cpp:808-817 |
| **ProbCut** | β + margin (108 default); quiescence probe → alphabeta | search.cpp:635-659 |

## Move ordering & history

| Structure | Detail |
|---|---|
| **Butterfly HH** | `history[side][threatSquare][from][to]` — **keyed on threatSquare**, not piece | search.cpp:128, RubiChess.h:1693 |
| Continuation history | `conthistptr[ply-1/2/4]` 3-ply depth (SF uses 2-ply) | search.cpp:130 |
| **Tactical history** | Separate `tacticalhst[piece_type][to][capture_type]` table (vs SF's folded) | search.cpp:159-181 |
| Capture ordering | MVV/LVA macro; bad captures flagged with BADTACTICALFLAG = bit 31 | RubiChess.h:1384-1386 |
| **Correction history** | Pawn-hash AND nonpawn-hash keyed (not piece × square): `pawncorrectionhistory[side][pawnhash & MASK]` + `nonpawncorrectionhistory[stm][nonpawnhash & mask]` | search.cpp:192-197 |
| Update gravity | `value · (1<<5) − hist · |value| / (1<<8)` (shift 5 new, shift 8 age) | search.cpp:144 |
| Threat computation | `updateThreats<Color>()` populates threat bitboard; cached in movestack | RubiChess.h:1837 |

## NNUE (dual architectures!)

### V5 (current, HalfKAv2_hm)
- Features: 64 × 11 × 64 / 2 = 22016 (after horizontal mirror)
- L1: 512 (SqrClipped + concat)
- L2: 16, L3: 32, output 1
- PSQT buckets (material-dependent)
- Net file: `nn-bc638d5ec9-20240730.nnue` (20 MB)

### V1 (HalfKP legacy)
- Features: 64 × 10 × 64 = 40960
- L1: 256 × 2 (ClippedReLU)
- L2: 32, L3: 32, output 1
- No bucketing

Detected by feature hash on load (`nnue.cpp:944-946`). Output scale: `nnuevaluescale` (default 59) → `eval = fwd · (600·1024 / scale) / (127·64)`.

## Time management
- **Soft (`endtime1`) vs hard (`endtime2`) limits** — asymmetric allocation with factors `f1` (128-256 scale) and `f2`
- Move anticipation via `bestmovenodesratio` + `movevariation` penalty
- Built-in **dynamic overhead calibration** (engine.cpp:242)
- NPS-limiting support for engine play
- More sophisticated than SF's simpler allocator

## Top 3 ideas for Hypersion

1. **Threat-square keyed history** (`search.cpp:128`, `RubiChess.h:1565`) — `history[side][threatSquare][from][to]` + `updateThreats<Color>()` early in search. Encodes positional context Stockfish doesn't capture. ~16 MB extra memory.
2. **Correction history keyed on material-hash** (`search.cpp:192-197`) — `pawncorrectionhistory[side][pawnhash & mask]` + `nonpawncorrectionhistory[stm][nonpawnhash & mask]`. Clusters by material imbalance snapshot (not per-position), reduces overfitting.
3. **Adaptive history extension threshold** (`search.cpp:808-817`) — auto-adjusts in [9, 15] based on extension-to-quiet-move ratio. Self-tuning vs SF's static threshold.

## Anti-patterns / cautions
- **Threat computation per non-leaf node** (`search.cpp:533-535`) — high branching factor accumulates cost. Profile before porting.
- **Dual NNUE architecture (V1 + V5)** bloats code. Drop V1 unless you specifically need it.
- **Correction history clamp ±8192** is aggressive — could lose information in extreme positions.
- **History age shift (1<<8)** can zero out old entries quickly in long games.
- **SEE early-exit pattern** — doesn't prune quiet moves, only captures fail this check.

## Code quality
Generally clear, modular. Template-heavy but well-structured movegen. Namespace isolation (`rubichess::`). Consistent naming. Debug infrastructure (SDEBUG, STATISTICS, TBDEBUG macros). All parameters exposed via `searchparamset` (~30+ tunable values). Moderate porting difficulty — threat-square history and dual-NNUE architectures are non-trivial.
