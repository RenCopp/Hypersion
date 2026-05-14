## Hypersion Texel Tuner

Coordinate-descent Texel tuner for Hypersion's classical eval. Reads
labeled positions from `<fen> | <result>` files, then sweeps the
tunable scalars in `src/eval_params.h::Params` to minimize the MSE
between Hypersion's classical eval (passed through a sigmoid) and the
labels.

**Status as of 2026-05-14**: fully operational. 145 tunable scalars,
OpenMP-parallel MSE (~7x speedup on 6-core hosts), 11 isolation flags
for category-specific re-tunes, automated apply via
`testing/apply_tuned_values.py`.

## Quick start

```
make tuner ARCH=x86-64-avx2
tuner --in testing/tuner_positions.txt --tune > tuner_run.txt 2>&1
py testing/apply_tuned_values.py testing/tuner_run.txt
```

Output ends with:
```
=== TUNED VALUES (paste into src/eval_params.h) ===
    int IsolatedPawnPenalty   = 33;
    int DoubledPawnPenalty    = 0;
    ...
Final MSE: 0.097965 (started 0.098120, gain 0.000155)
```

`apply_tuned_values.py` parses that block and rewrites
`src/eval_params.h` in-place (backup at `eval_params.h.bak`).

## Data file format

One record per line:
```
<fen> | <result>
```
`<result>` is `1` (white won), `0` (black won), or `0.5` (drew).

## Pre-built datasets

| File | Size | Source | Use case |
|---|---:|---|---|
| `testing/tuner_positions.txt` | 12 MB | 221k Hyp self-play | Quick tunes, tactical-aligned |
| `testing/tuner_positions_3M.txt` | 180 MB | 3M Encroissant master games | Intermediate |
| `testing/tuner_positions_large.txt` | 993 MB | 16M Encroissant master games | Definitive |

## Building a new dataset

```
make pgn_to_positions ARCH=x86-64-avx2
pgn_to_positions --in some.pgn --out positions.txt \
    --every 6 --skip-opening 8 --skip-tail 6 \
    --max-records 20000000
```

Walks SAN move text, samples one position every N plies, labels with
game result. The 16M dataset takes ~20 min to extract from the 12.8 GB
Encroissant 2025 PGN on a fast NVMe.

## Tune modes

The default mode tunes ALL 145 knobs at once (~17h on 16M). For
faster iteration, use category-isolation flags:

| Flag | Knobs | Description |
|---|---:|---|
| (default) | 145 | Full tune (all knobs) |
| `--psqt-only` | 12 | R10 per-piece-type PSQT scalars |
| `--pval-only` | 10 | R13 piece-value scalars (tight 85-120) |
| `--passed-only` | 3 | R14 PassedRankBonus[4,5,6] |
| `--mob-only` | 8 | R5 per-piece-type mobility scalars |
| `--threats-only` | 11 | R16 ThreatByMinor/Rook/Pawn |
| `--king-only` | 8 | R17 KingAttacker + SafeCheck weights |
| `--shelter-only` | 3 | R18 PawnShelter_NMissing |
| `--init-only` | 6 | R22 Initiative bonus coefficients |
| `--scale-only` | 4 | R21/R25/R26 endgame scale percents |
| `--new-only` | 19 | R22-R31 joint (recommended for new-feature tunes) |
| `--part2-only` | 10 | R32-R37 joint (still disabled-at-0 by default) |

Categorical tunes are 10-50x faster than the full tune. The
**`--new-only` joint mode** is the WAC-184 winner — single-knob tunes
escape its local optimum and regress tactical play.

## Stage progression

Recommended workflow when adding new features:

1. **Add knob** to `src/eval_params.h::Params` and corresponding eval
   code in `src/evaluate.cpp`. Default value 0 (disabled).
2. **Add knob entry** to `tuner.cpp::knobs[]` AND to the appropriate
   isolation group (e.g., `new_knobs[]`, `part2_knobs[]`).
3. **Tune in isolation** first (e.g., `--part2-only`) on the 221k
   self-play dataset (~3 min) to find rough magnitudes.
4. **Validate with WAC** via `py testing/wac_runner.py --depth 8 --no-nnue`.
   If WAC < 178, the feature regresses tactical play — see "MSE vs
   WAC mismatch" below.
5. **Re-tune jointly** with related features (`--new-only`) on 16M
   master games (~3h) to find a coordinated optimum.
6. **Apply** via `apply_tuned_values.py` and rebuild.

## Critical: MSE vs WAC mismatch

The Texel MSE objective optimizes for predicting game results on
the input dataset. This DOES NOT always preserve depth-8 tactical
sharpness. Empirically (this project, 2026-05-13/14):

- 16M master-game tunes can lower MSE by ~0.0007 while regressing
  WAC by 3-8 points.
- Tighter ceilings reduce regression magnitude but rarely eliminate it.
- Self-play (Hyp) dataset is more tactically-aligned (221k positions)
  but smaller — slightly less signal-to-noise but values transfer
  better to actual play.

**Always validate tuned values with WAC depth-8 BEFORE shipping.**
If WAC regresses, either:
- Revert to previous values (most common)
- Disable the new feature at 0 (R32-R37 are shipped this way)
- Tune jointly with adjacent features (R22-R31 `--new-only` worked)

## Critical: stale-obj-file hang

Adding new struct fields to `src/eval_params.h` changes the binary
ABI. Incremental `make` may NOT relink all object files, causing
segfaults / hangs at depth 7+ in the search.

**Symptom**: `bench` outputs depth 1-6 of position 1, then no
`bestmove`, then process hangs forever.

**Fix**: `make clean && make -j` after every eval_params.h struct
field addition.

## Internals — coordinate-descent algorithm

```
for sweep = 1..24:
    changed = false
    for each knob k:
        for step_size in {1, 2, 4}:
            for delta in {-step_size, +step_size}:
                old = k.value
                k.value = old + delta
                if k.value < k.floor or k.value > k.ceil: continue
                new_mse = mse(records)
                if new_mse < best_mse:
                    best_mse = new_mse
                    changed = true
                    break (try next knob)
                else:
                    k.value = old
    if not changed: break
report final values
```

The step-size escalation (1 -> 2 -> 4) lets the tuner walk faster
when far from optimum but settle precisely near it.

`-fopenmp` parallelizes the per-position MSE computation across
threads. Each MSE eval over 16M positions takes ~6 sec on 6 cores.

## Internals — knob ceiling pinning

If a tuned value pins at its declared ceiling (e.g., `Scale = 200`
at `ceil = 200`), the tuner couldn't find a higher-MSE direction;
the true optimum may be higher. Widen the ceiling and re-run.

Empirically, **multi-knob ceilings pin TOGETHER** — widening one
without widening neighbors usually doesn't help. The R22 widened-
ceiling experiment (2026-05-14) widened all 6 R22 knobs at once
and found a new optimum, but that optimum regressed WAC -3 vs the
joint-tuned values.

## Expected gain

| Stage | MSE start | MSE end | Gain | Real-game ELO |
|---|---:|---:|---:|---:|
| First tune over 14 base scalars on 221k | ~0.105 | ~0.104 | 0.001 | +20-40 |
| R20 added features (R10-R21) on 16M | ~0.104 | ~0.196* | n/a | +50 (cumulative) |
| R22-R31 `--new-only` joint on 16M | 0.195389 | 0.195198 | 0.000226 | +0-3 WAC |
| R32-R37 `--part2-only` on 16M (rejected) | 0.195198 | 0.195128 | 0.000070 | -8 WAC |
| R22 widened ceilings (rejected) | 0.195198 | 0.195137 | 0.000061 | -3 WAC |

\* MSE scale changed when piece-value tuning was added (R13); not
directly comparable to pre-R13 numbers.

After ~20 rounds of features, the engine is at a tight local optimum.
Single-knob tunes typically regress when applied. The path forward
is **joint SPSA over coordinated parameter clusters**, not isolated
tune-and-apply cycles.

## Build

```
make tuner ARCH=x86-64-avx2 -j
```

Links against `src/{bitboard,position,movegen,zobrist,misc,evaluate}.cpp`
plus stubs for NNUE (the tuner only uses classical eval). OpenMP via
`-fopenmp`.

## Files

- `tools/tuner/tuner.cpp` — entry point, knob declarations, isolation
  flags, coordinate-descent loop.
- `tools/tuner/pgn_to_positions.cpp` — PGN to FEN sampler.
- `testing/apply_tuned_values.py` — parses tuner output, rewrites
  `src/eval_params.h`.
- `src/eval_params.h` — 145 tunable scalars in `Params` struct.
