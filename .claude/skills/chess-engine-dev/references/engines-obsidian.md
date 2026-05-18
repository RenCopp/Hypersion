# Obsidian — reference notes for Hypersion development

## What it is
Competitive C++ engine by Gabriele Lombardo. ~top-100 CCRL territory, ~3500 ELO. Known for **aggressive threat-aware move ordering** + **specialized NNUE Finny caching**.

## Source location
`C:\Engine\Engines\Obsidian-16.0\Obsidian-16.0\` — extracted. Prebuilt avx2 at `C:\Engine\Engines\prebuilt\obsidian\Obsidian.exe` (~32 MB self-contained — net embedded).

## Distinctive search heuristics

| Heuristic | Detail | File:line |
|---|---|---|
| **LMR formula** | `lmrBase=94, lmrDiv=314`; `LMR = base + log(d)·log(m)/div` — pure log scaling | search.cpp:126 |
| **NMP** | `R = min((eval−β)/147, EvalDivMin) + d/depthDiv + 4 + ttMoveNoisy` — **aggressive eval scale (147)** | search.cpp:878 |
| **Razoring** | `eval < α − 352·d` (depth-scaled threshold) | search.cpp:852 |
| **RFP** | `eval ≥ β + max(87·(d−impr), 22)` (RfpMaxDepth=11) | search.cpp:863 |
| **Singular extension** | `sBeta = ttScore − (d · 121) / 64`. **Triple extension** (`TripleExtMargin=121`) and **negative extensions** (`-3 + IsPV` if ttScore ≥ β) | search.cpp:1035, 1038-1052 |
| **History pruning** | Quiet pruned if `history < -7471·d` — aggressive negative threshold | search.cpp:1001 |
| **Futility pruning** | `eval + 159 + 153·lmrDepth ≤ α` | search.cpp:1015 |
| **Multicut SE** | If `singularBeta ≥ β` → return singularBeta directly (rare in practice) | search.cpp:1047 |

## Move ordering & history

| Structure | Detail |
|---|---|
| Main history | `[2][64²] int16` per-side from-to |
| Capture history | `[PIECE_NB·SQ_NB][PIECE_TYPE_NB]` |
| Continuation history | `[2][PIECE_NB·SQ_NB][PIECE_NB·SQ_NB]` at offsets {1, 2, 4, 6} |
| **Pawn history** | `[1024][PIECE_NB·SQ_NB]` keyed on pawn hash modulo 1024 — **unique** |
| **Correction history** | PawnCorrHist + NonPawnCorrHist; piecewise scaling by abs(value)/1024 |
| Counter move | `[PIECE_NB·SQ_NB]` |
| **Threat-weighted quiet scoring** | `calcThreats()` precomputes byPawn/byMinor/byRook bitboards. Quiets get **±32768 (QUEEN), ±16384 (ROOK/MINOR)** based on moves into/out of threat squares | movepick.cpp:61-98, position.cpp:380-408 |

**LMR history divisors**: 3516 (early), 9621 (quiet), 5693 (capture) — narrower than SF's 8000-11000.

## NNUE
- Architecture: 768 → 1536 (L1) → 16 (L2) → 32 (L3) → 1
- **13 king buckets** (more than SF's 8), 8 output buckets (material-based)
- NetworkScale=400, QA=255, QB=128
- **Finny table**: 2 × 13 buckets `[fileOf(king)≥E][bucket]` — pre-cached accumulators per king bucket
- **doUpdates** supports NORMAL/CAPTURE/CASTLING optimized paths with `multiSubAdd`, `multiSubAddSub` SIMD helpers
- **L1Weights double-buffering** (`L1WeightsAlt`) for DPBUSD-friendly transpose without runtime permutation

## Time management
- Soft: `optimum = min(0.95/mtg, 0.88·time/timeLeft)`; cap 2.5% of remaining when mtg==0
- Hard: 80% of time − overhead
- Root-level scaling: `tm0-tm6` + `lol0-lol1` parameters
- **Stability-weighted stop**: `elapsed > stability · nodes_factor · score_factor · optimum` → stop. Uses searchStability (0-8) + (1 − bestMoveNodeFraction) + score-change.

## Top 3 ideas for Hypersion

1. **Pawn-keyed history** (`history.h:23`) — `pawnHistory[pawnKey & 1023][pieceTo]`. Captures pawn-structure-aware move ordering. Simple, cheap lookup.
2. **Threat-weighted quiet scoring** (`movepick.cpp:77-98`, `position.cpp:380-408`) — `calcThreats()` + ±32K/±16K penalties for quiets entering/leaving threat squares. Catches queen-hangs early in move ordering. Synergizes with SEE.
3. **Finny accumulator cache** (`nnue.h:81-82`) — 2 × 13 cached L1 accumulators per king bucket. Avoids full refresh on king moves within same bucket. Already partially present in Hypersion as `g_finny`; Obsidian's 13-bucket variant is more granular than Hypersion's current 8.

## Anti-patterns / cautions
- **Full position copy per move** (`search.cpp:1055`): `Position newPos = pos; playMove(newPos, move, ss);` — copies entire board+metadata on every move (no make/unmake). Compensated by inlining but structurally weaker than incremental.
- **Hardcoded threat magnitudes** (32768/16384) lack scaling by phase or piece count.
- **`tm0`-`tm6` heavily TC-tuned** — tournament-fit, may not transfer.
- **SE multicut condition rarely fires** (`singularBeta ≥ β`) — likely dead code in practice.
- **`goto` in movepicker** (`movepick.cpp:139, 150, 196`) — valid state-machine but non-idiomatic C++.

## Code quality
Clean modular layout (search/movepick/nnue/timeman separated). DEFINE_PARAM macro for tuning visibility. SIMD with AVX512/AVX2 fallback. Many magic constants but well-organized. Manual NNUE accumulation (no template metaprogramming bloat). Moderate-to-good port suitability — flat architecture makes individual heuristics portable, but NNUE tightly coupled to Finny caching scheme.
