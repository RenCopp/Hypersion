# SF18 NNUE architecture (SFNNv10)

## Network topology

Big net `nn-c288c895ea92.nnue` (102MB):
```
input(102384) → FT(1024) → squared-clipped-ReLU → L2(15) → L3(32) → out(1)
                                                  + skip channel from L2[15]
```

- **FT (feature transformer)**: HalfKAv2_hm + FullThreats. Input dim 102384 =
  PSQ_DIM (22528) + THREAT_DIM (79856 in SF18, varies). Output 1024 ints (int16).
- **HalfKAv2_hm**: HalfKA with horizontal-mirror symmetry; halves number of
  king-square buckets.
- **FullThreats**: per-square attack/defense bitmaps as features. Big-net only.
- **L1 transform**: pairwise multiply two halves of FT output, clipped/squared,
  reduces 1024 → 1024 (effectively halved by structure). Output is uint8.
- **L2/L3/output**: small fully-connected layers. int8 weights, int32 accumulators,
  shifted-and-clipped between layers.
- **PSQT bucket**: parallel scalar prediction from FT output, 8 buckets keyed by
  popcount. Combined with positional eval as `nnue = (125·pv + 131·pp) / 128`.

Small net `nn-37f18f62d772.nnue` (3MB):
- Same shape but FT(128) instead of FT(1024). No threat features.
- Used when material is lopsided (`use_small()` returns true).
- If small net says |v| < SMALL_FALLBACK_TH, fall back to big net.

## Net selection (Hypersion: src/nnue.cpp:1278-1340)

```cpp
// SF18 evaluate.cpp:43-70 logic, ported:
//   1) if use_small(pos): try small net
//   2) if abs(small_eval) < SMALL_FALLBACK_TH: recompute with big
//   3) else: use small_eval
```

Hypersion's threshold differs from SF18's 962 — currently 1500. Lower
threshold means small net used more often (faster but slightly less
accurate). Tuning this is a known sensitivity.

## Forward pass flow (src/nnue.cpp::evaluate)

1. **Refresh accumulator** from incremental update or finny cache.
2. **L1 transform**: pairwise s0·s1 → uint8 buffer (squared-clipped-ReLU).
   In Hypersion: AVX2 SIMD using `cvtepi16_epi32`, `max`, `min`, `mullo`,
   `srli`, `packus`. Commit 3e69bcb.
3. **FC0** (1024 → 16): `simd_dot_i8_u8` per row, uses `dpbusd` if VNNI else
   `maddubs+madd`. Bias is int32.
4. **L2 transform**: o0[i] squared/127 + clipped → 30 uint8 input bytes.
5. **FC1** (30 → 32): same SIMD pattern. NB: input padded to 32 (fc1pi=32).
6. **L3 transform**: clipped → 32 uint8 input bytes.
7. **FC2** (32 → 1): single dot product. Bias-add, output the positional eval.
8. **Skip channel**: `o0[15] * (600 * OUTPUT_SCALE) / (127 * (1 << WSCALE))`
   added to FC2 output to give "raw" positional.
9. **Combined**: PSQT-bucket eval + positional, weighted 125:131/128.
10. **Material scaling**: `mat = 534·#P + 776·#N + 825·#B + 1276·#R + 2538·#Q`,
    final eval scaled by `(750 + mat/32) / 1024`.

## Incremental update (FinnyCache)

- Cached accumulators per (king-square, perspective) → avoid full re-refresh
  on king moves. SF18 calls these "finny tables".
- Hypersion: `thread_local FinnyCache g_finny;` (src/nnue.cpp:530).
- Invalidated on `unload()` and `new_game()` only — NOT between bench positions
  (each bench position spawns a new thread, so `g_finny` is fresh per-position).

## SIMD types

| Intrinsic | What | Where Hypersion uses it |
|---|---|---|
| `_mm256_dpbusd_epi32` | int8·uint8 dot product, accumulate to int32 | FC0, FC1, FC2 (VNNI fast path) |
| `_mm256_maddubs_epi16 + _mm256_madd_epi16` | non-VNNI fallback for above | same FC layers |
| `_mm256_mullo_epi32` | 32-bit lane multiply | L1 transform (s0·s1) |
| `_mm256_srli_epi32` | shift right | L1 (>>9), output (>>WSCALE) |
| `_mm256_packus_epi32 + packus_epi16` | i32 → u8 with saturation | L1 transform output |

VNNI = AVX-VNNI (Alder Lake+) or AVX-512-VNNI. Hypersion build flags:
`-march=haswell` does NOT enable VNNI; `-march=alderlake` or `-mavxvnni` does.

## Optimization gotchas

- **Bench is non-deterministic** (open bug). Don't compare bench NPS as
  validation — use SPRT.
- **Single-process bench varies ±25-40%** even at Threads=1. Get 5+ runs.
- **Concurrency=6 masks NNUE optimizations** by ~50% (cache contention).
  Use concurrency=2 for memory-aggressive tests.
- **Scrambled FC weight layout** (col/4·padded·4 + row·4 + col%4) was
  byte-equivalent but +5.2 ± 37.9 ELO neutral. See nnue.cpp tombstone.
