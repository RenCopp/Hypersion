# Ethereal — reference notes for Hypersion development

## What it is
Long-running C engine by Andrew Grant. v14.00 era (~2019-2020). **Classical eval + optional NNUE** hybrid: NNUE used when `|classicalPSQT| ≤ 2000`, classical otherwise. ~3100 ELO peak. Most readable open-source classical eval reference available.

## Source location
`C:\Engine\Engines\Ethereal-14.00\Ethereal-14.00\src\` — extracted. Builds via clang+PGO (currently fails on MSYS2 with bitcode-link error; **no prebuilt Windows binary**, not in tournament).

## Distinctive search heuristics

| Heuristic | Detail | File:line |
|---|---|---|
| **LMR formula** | `0.75 + log(d)·log(played) / 2.25` (hand-tuned logarithm, not tabulated) | search.c:155 |
| **NMP gate** | `eval ≥ β AND (ns-1).move ≠ NULL AND depth ≥ 2` (simpler than SF) | search.c:475-485 |
| **NMP reduction** | `R = 4 + d/6 + (eval−β)/200 + tactical` | search.c:485 |
| **ProbCut** | depth ≥ 5, fixed margin = 100, reduces by 4 at full depth | search.c:499-532 |
| **Singular extension** | depth ≥ 8, ttMove only (narrower gates than SF) | search.c:618-628 |
| **Aspiration depth reduction on fail-high** | `depth = depth − (\|score\| ≤ MATE/2)` — **unusual**, most engines hold depth steady. Premature depth cut could miss best move. | search.c:280 |

## Move ordering & history

| Structure | Detail |
|---|---|
| **Threat-based butterfly** | `HistoryTable[COLOUR][2][2][SQ][SQ]` (~128K int16s). 2D threat-flag indexing: (threat_from, threat_to). **More granular than SF's color-based.** |
| Capture history | `CaptureHistoryTable[PIECE][2][2][SQ][PIECE-1]` (~5K) |
| Continuation history | 2-ply depth (CM + FM); per-piece, per-dest |
| Counter moves | Per-side, per-piece, per-dest |
| **MVV-LVA dynamic boost** | Added during move scoring (history.c:146): +2400 minor, +4800 rook |
| Bonus formula | `depth > 13 ? 32 : 16d² + 128·max(d−1, 0)` (Stockfish formula) |
| Gravity divisor | 16384 |

Move picker stages (9): TABLE → GEN_NOISY → GOOD_NOISY → KILLER1/2 → COUNTER → GEN_QUIET → QUIET → BAD_NOISY → DONE. **SEE threshold applied during GOOD_NOISY** — moves below threshold marked -1 and deferred.

## Classical eval (Ethereal's claim to fame)
Highly readable, named-constants throughout `evaluate.c`. Major terms:

- **Material**: P=S(82,144), N=S(426,475), B=S(441,510), R=S(627,803), Q=S(1292,1623)
- **PSQT**: full 64-square tables per piece, merged into PSQT[32][64] at init
- **Pawn structure**: Candidate passers (per rank), Isolated, Stacked, Backwards (per rank), Connected32 (32 pair patterns)
- **Knight**: Outpost[2][2], BehindPawn, **InSiberia[4]** (A-file corners), Mobility[9]
- **Bishop**: Pair, RammedPawns, Outpost[2][2], BehindPawn, LongDiagonal, Mobility[14]
- **Rook**: FileType[2], OnSeventh, Mobility[15]
- **Queen**: RelativePin, Mobility[28]
- **King safety**: PawnFileProximity[8], Defenders[12], Shelter[2][8][8], Storm[2][4][8]
- **Threats**: WeakPawn, MinorAttackedBy{Pawn/Minor/Major/King}, RookAttackedByLesser, QueenAttackedByOne, Overloaded, ByPawnPush
- **Space**: RestrictPiece, RestrictEmpty, CenterControl
- **Complexity**: TotalPawns, PawnFlanks, PawnEndgame, Adjustment
- **Scale factors**: OCB_BishopsOnly=64, OCB_OneKnight=106, OCB_OneRook=96, LoneQueen=88, LargePawnAdv=144

**Pawn-king cache**: separate hash table (`ei->pkentry`) caches pawn-king eval for amortized reuse.

## NNUE
- 20480 input features → 512 KP layer → 1024 L1 (int8 quantized) → 16 L2 (float, after delayed dequant) → 16 L3 → 1
- **Delayed dequantization**: L2 biases upshifted by `(1<<SHIFT_L1)=64` at init, saving SRAI per eval call.
- Quantization shifts: `shift_l0=6, shift_l1=5`
- Blending at `evaluate.c:449`: `if USE_NNUE && |psqt_eg| ≤ 2000 → NNUE, else classical`

## Top 3 ideas for Hypersion

1. **Threat-based history indexing** (`history.c:79`, `types.h:103-105`) — `(threat_from, threat_to)` 2D bucket. More granular than SF's color-based. ~128 KB memory cost.
2. **Pawn-king eval caching** (`evaluate.c:449-460`) — `ei->pkentry` separate hash for amortized pawn-structure compute. Already partially present in Hypersion via `pawn_hash.h` (header drafted, full integration deferred per CLAUDE.md).
3. **Delayed NNUE dequantization** (`nnue.c:60-71`) — Upshift L2 biases at init instead of SRAI per eval. Distinct from SF's accumulator handling.

## Anti-patterns / cautions
- **Aspiration depth reduction on fail-high** (`search.c:280`) is **questionable**; premature cut could miss best move. Don't port without SPRT.
- **Simpler NMP without piecewise eval clipping** — less sophisticated than SF18; works but may miss optimization.
- **Threats stored in board state** — adds memory state vs SF's on-demand compute. Trade memory vs CPU.

## Code quality
Excellent readability. Every eval term named with magic constants labeled. File headers explain purpose. Clean modularization (eval.c / pawn-king cache / search.c / nnue separate). Constants cite ELO impact in comments. C89/C11 standard, no heavy dependencies.

Best engine in this set for **learning classical eval design** — read `evaluate.c` line-by-line for terms.
