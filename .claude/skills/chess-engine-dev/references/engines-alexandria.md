# Alexandria — reference notes for Hypersion development

## What it is
Modern C++ UCI engine by PGG106. Multi-bucket NNUE architecture (768 → 1536×16-buckets → 16/32×dual → 1×8-buckets). ~3400 ELO. Designed for OpenBench tuning with 50+ exposed parameters.

## Source location
`C:\Engine\Engines\Alexandria-master\Alexandria-master\` — extracted from `Alexandria-master.zip`. Prebuilt avx2 binary at `C:\Engine\Engines\prebuilt\alexandria\Alexandria.exe`.

## Distinctive search heuristics

| Heuristic | Detail | File:line |
|---|---|---|
| **Hindsight reduction** | `depth -= 1` if prev_reduction ≥ 1 AND (ss-1).staticEval + current.staticEval ≥ tuned threshold | search.cpp:553 |
| **RFP with IIR penalty** | margin = `rfpDepthMargin*d - rfpImprovingMargin*impr - rfpIIRMargin*iir` (3-term, vs SF's 1-term) | search.cpp:391 |
| **NMP verification** | At depth ≥ 15, re-search after NMP cutoff (similar to SF18 but `R = 4 + d/3 + min((eval−β)/221, 3)`) | search.cpp:575 |
| **Complexity-aware LMR** | If `\|rawEval − NNUE\| > 50`, reduce_less by 1. Lightweight signal of tactical complexity. | search.cpp:812 |
| **ProbCut improving margin** | `β = α + 287 − 54·improving` (SF uses fixed) | search.cpp:615 |

## Move ordering & history

| Structure | Detail |
|---|---|
| Butterfly | `int16[2][64²]` HH with tuned bonus/malus/offset/multiplier |
| Capture history | independent bonus/malus curves; piece × to × victim |
| Continuation history | offsets {1, 2, 4, 6} ply back; **TT malus** of `-155·depth` (capped) |
| Pawn history | hash(pawnKey) → [dest_piece]; piece-based not square-based |
| **Correction history** | Triple source: pawn, minor, nonPawn(W/B); weights 29/34/26 tuned separately |
| Root history | Separate 2×4096 table tuned independently from main HH |

## NNUE
- Architecture: `768×16 → 1536`×2 → 16 → 32 → 1×8-buckets
- **Dual activation**: L2 outputs doubled (standard + alternate activation paths)
- King buckets: 16 (8×8 file-symmetric)
- Output buckets: 8
- Quantization: FT=255, L1=64; **L2/L3 are float** (not int)
- NET_SCALE = 362
- FinnyTable: per-king-bucket, per-side L1 accumulator cache, 64-byte aligned

## Time management
**Multi-factor adaptive scaling** (3 independent factors):
1. `bestMoveScale`: 5-level curve based on best-move stability (0-4 iterations)
2. `evalScale`: 5-level curve based on eval stability (±10cp window)
3. `nodeScale`: `(nodeTmBase − bestMoveNodesFraction) · nodeTmMultiplier`

Hard cap: 76% of remaining time.

## Top 3 ideas for Hypersion

1. **Hindsight reduction** (`search.cpp:553`) — depth penalty after detecting prev ply over-reduced. Low overhead, orthogonal to existing LMR. ~6 ELO claimed in code comments.
2. **Multi-factor adaptive time** (`time_manager.cpp:43-53`) — three independent scaling factors beat single-margin model. Tuning-friendly.
3. **Complexity-aware LMR** (`search.cpp:812`) — `\|rawEval − NNUE\| > 50` → reduce_less. One subtract + cmp, prevents over-reduction in tactical positions.

## Anti-patterns / cautions
- **Parameter explosion** (50+ tunables) — overfitting risk if tuned on narrow TC range.
- **Hindsight reduction lacks rootNode guard** — could inflate NPS without depth gain in some configs.
- **Correction history weights (29/34/26) unpublished** — no ablation study visible.
- **Aspiration delta×1.44 on fail** — no documented reasoning.

## Code quality
Very high readability. Macro-based `TUNE_PARAM` system declares min/max/C_end/R_end per parameter for OpenBench. Portable across MSVC/GCC. NNUE embedding has MSVC fallback (no incbin).
