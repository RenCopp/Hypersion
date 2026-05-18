# Berserk — reference notes for Hypersion development

## What it is
Top-tier C engine by Jay Honnold. ~3650-3700 ELO (CCRL 40/15). Known for **NNUE eval corrections** blended with raw output, **sparse L1** SIMD optimization, and **measured-tuned** search constants.

## Source location
`C:\Engine\Engines\berserk-main\berserk-main\` — extracted. Prebuilt avx2 at `C:\Engine\Engines\prebuilt\berserk\Berserk.exe` (uses `berserk-d43206fe90e4.nn` alongside).

## Distinctive search heuristics

| Heuristic | Detail | File:line |
|---|---|---|
| **LMR formula** | `log(d) × log(m) / 2.1350 + 0.2319` (vs SF's `d × m / 2.4`) | search.c:52 |
| **LMP non-improving** | `1.77 + 0.98·d²` (SF: ~0.5·d²) | search.c:61 |
| **LMP improving** | `1.43 + 0.33·d²` (SF: ~1.5·d²) | search.c:60 |
| **RFP** | `eval − 83d + 148·impr + 25·declining ≥ β` (3-term) | search.c:521 |
| **NMP linear-R** | `R = 4 + 367d/1024 + min(9(e−β)/1024, 4)` — **integer arithmetic, linear** vs SF's quadratic | search.c:538 |
| **Singular extension** | Score-tiered: 3-ply if depth ≤7 and score < sBeta−43; 2-ply if depth ≤6 and < sBeta−12; else 1-ply | search.c:683-691 |
| **LMR ⋅ history** | `R -= 8·history / 65536` (higher divisor = milder LMR adjustment vs SF's `4·hist/256`) | search.c:625 |
| **Cutnode reduction** | `R += 1 + !IsCap(move)` (SF: `R += 2`) | search.c:736 |
| **50-move rule eval damping** | `AdjustEvalOnFMR` penalizes eval as rule50 rises (drawish positions); good empirical win | search.c:81-83 |

## Move ordering & history

| Structure | Detail |
|---|---|
| Butterfly HH | `int16[2][2][2][64²]`: stm × from_threatened × to_threatened × from-to. **3D threat-flag indexing.** |
| Continuation history | Depths {-1, -2, -4, **-6**} (SF typical: 2 or 4-deep) |
| Capture history (CVH variant) | `[12][64][2][7]`: piece × to × **defender_status** × victim_type. Unusual 2D defender bucket. |
| Counter moves | Per-side butterfly `[12][64]` |
| **History bonus** | `min(1708, 4d² + 191d − 118)` — quadratic decay (SF: linear) |
| Gravity divisor | 16384 (2^14, standard) |

Move picker stages: TT → Good noisy (SEE ≥ thr) → Killers → Counter → Quiet → Bad noisy.

## NNUE
- Architecture: HalfKA-style, 16 king buckets × 12 pieces × 64 squares = 12288 input features → 1024 hidden per perspective (2048 activation total) → L1 affine to 16 → L2 to 32 → output
- **Quantization**: int16 hidden (5-bit shift), int8 L1 (12-bit biases), int16 L2/L3
- **Sparse L1 with NNZ lookup**: AVX512/AVX2/NEON/scalar paths. Tracks non-zero activation indices, skips zero-weight multiplications.
- King buckets: 16, mapped via `KING_BUCKETS[64]` (file-based, corner-heavy)
- **Evaluation corrections** (key insight!): `eval = rawEval + (31·pawnCorr + 17·cont1 + 46·cont2) / 8192`
  - `pawnCorrection[131072]` keyed by pawn zobrist
  - `contCorrection[12][64][12][64]` for continuation pairs

## Time management
**3-factor scaling** (no easy-move detection):
- `stabilityFactor = 1.3110 − 0.0533 · searchStability`
- `scoreChangeFactor = 0.1127 + 0.0262·d3_diff + 0.0261·d_prev_diff` (improving only)
- `nodeCountFactor = max(0.563, 2.2669·(1 − bestNodesFrac) + 0.4499)`
- `max_time = max(Limits.alloc · stability · score · nodes, hardCap)`

## Top 3 ideas for Hypersion

1. **Eval correction blending** (`history.h:79-85`, `types.h:203-204`) — pawn + continuation history corrections applied post-NNUE. Highest ELO impact, lowest risk. Cheap table lookups + weighted blend.
2. **LMR formula with history adjustment** (`search.c:52, 625`) — `4d² + 191d − 118` LMR cap + `8·history/65536` divisor. Empirically tuned; worth A/B vs Hypersion's current LMR.
3. **NMP linear-R interpolation** (`search.c:538`) — `R = 4 + 367d/1024 + ...` integer arithmetic. Shallower at low depth, steeper at high depth; may fit Hypersion's profile better than quadratic.

## Anti-patterns / cautions
- **Continuation history at -6** adds 64 B/thread; minor ELO, evaluate cost.
- **3-factor time mgmt** is empirically fit to Berserk; **6 coefficients must be re-tuned** for any port. SF's simpler model more robust without tuning.
- **Capture history with defender_status indexing** — non-standard 2D layout; not obviously superior to SF's (piece, victim).
- **Sparse L1 NNZ** requires conditional AVX512 path; high dev cost.

## Code quality
Modular and readable. Strong SIMD-agnostic design (5 code paths). Terse macros (`HH()`, `TH()`, `FeatureIdx()`) require getting used to. No platform hacks outside SIMD detection.
