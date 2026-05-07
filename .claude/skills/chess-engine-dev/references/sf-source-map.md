# Stockfish source map — `C:\Engine\stockfish\src\`

Where to find each concept in SF's source. Read these before porting.

## File index

| File | Purpose |
|---|---|
| `search.cpp`, `search.h` | Main search, iter-deepen, NMP/SE/LMR/ProbCut, time scaling |
| `evaluate.cpp`, `evaluate.h` | NNUE forward, net selection, material scaling |
| `movepick.cpp`, `movepick.h` | MovePicker stages, score_quiets/captures/evasions |
| `movegen.cpp`, `movegen.h` | Pseudo-legal move generation |
| `position.cpp`, `position.h` | Board state, do_move/undo_move, see_ge, repetition |
| `tt.cpp`, `tt.h` | TT cluster + 8-byte entry, probe/replace logic |
| `history.h` | History tables (in SF: in `search.h` typically; SF18: history.h) |
| `timeman.cpp`, `timeman.h` | Time-budget computation |
| `bitboard.cpp`, `bitboard.h` | Magic bitboards, attacks_bb<>, pseudo_attacks |
| `tune.cpp`, `tune.h` | TUNE() macros for SPSA tuning (compile-time toggle) |
| `engine.cpp`, `engine.h` | UCI command dispatch, option setting |
| `score.cpp` | Score → win-rate model (used in cp output) |
| `thread.cpp`, `thread.h` | ThreadPool, lazy SMP, helper-skip schedule |
| `numa.h` | NUMA topology / CPU affinity (Hypersion ignores) |

## Mapping to Hypersion

| SF file | Hypersion file |
|---|---|
| `search.cpp` | `src/search.cpp` (1:1 conceptual) |
| `evaluate.cpp` (small) | `src/evaluate.cpp` (small wrapper) |
| `nnue/*` (multi-file) | `src/nnue.cpp` (single file, mono-namespace) |
| `movepick.cpp` | `src/movepick.cpp` |
| `position.cpp` | `src/position.cpp` |
| `tt.cpp` | `src/tt.cpp` |
| `history.h` | `src/history.h` |
| `timeman.cpp` | `src/timeman.cpp` |
| `tune.cpp` | (no equivalent — Hypersion uses hand-tuned constants) |

## Common porting tasks

### Porting a new pruning heuristic

1. Read SF's `search.cpp` for the heuristic. Note depth/score gating.
2. Read Hypersion's `src/search.cpp` for surrounding context. Find where
   the SF heuristic would fit.
3. Check if the SF magnitudes need 5x scaling (SF: cp; Hypersion: cp*5).
4. Add behind a simple A/B switch (no compile flag — just a direct change).
5. Build, run 30g triage. Tombstone if neutral/negative.

### Porting a new history table

1. SF's `history.h` (in SF18 it's split between search.h and history.h).
2. Add to Hypersion's `src/history.h` as a struct with clear/decay/get/update.
3. Add member to `Worker` in `src/search.h`.
4. Add `clear()` call in `Worker::clear()`.
5. Add `decay()` call in `Worker::decay_for_new_game()`.
6. Wire reads/writes into search/movepick.

### Porting a NNUE-side change

1. SF's NNUE is in `src/nnue/` with multiple files (network, layers,
   feature_transformer, etc.). Map to Hypersion's monolithic `nnue.cpp`.
2. Note Hypersion's eval is at 5x scale — most NNUE outputs are NOT scaled,
   only the final UCI output divides by 5.
3. Test at concurrency=2 to avoid cache-contention masking.

## Quick-grep recipes

```
# Find all SF18 uses of `dpbusd`:
Grep("_mm256_dpbusd_epi32", path="C:\Engine\stockfish\src", type="cpp")

# Find SF's RFP margin formula:
Grep("rfpMargin|RFP_MARGIN|rfp_margin", path="C:\Engine\stockfish\src\search.cpp")

# Find SF's threat-by-lesser logic:
Grep("threatByLesser|threatByPawn|threatByMinor", path="C:\Engine\stockfish\src", type="cpp")

# Compare Hypersion vs SF18 LMR formula:
Read("C:\Engine\stockfish\src\search.cpp") at the Reductions[] init
Read("C:\Engine\Hypersion\src\search.cpp") line 47-58 (init() function)
```
