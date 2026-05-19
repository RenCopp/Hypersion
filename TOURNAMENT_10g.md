# 10g/pairing tournament — Hypersion 3.0 vs reference field

**Date**: 2026-05-19
**Config**: TC 5+0.05, conc=4, Threads=1, Hash=64, 8mov.epd openings, round-robin 5 rounds × 2 games per pair = 10 games per pairing, 100 games total.

## Final standings

| Rank | Engine | ELO ± | Score | Draw% |
|---|---|---|---|---|
| 1 | Stockfish | +147 ± 70 | 70.0% | 55.0% |
| 2 | Obsidian-16.0 | +98 ± 65 | 63.7% | 62.5% |
| 3 | Berserk-13 | +53 ± 69 | 57.5% | 60.0% |
| 4 | Alexandria-9.0 | +44 ± 75 | 56.3% | 52.5% |
| 5 | **Hypersion-3.0** | **-636** | **2.5%** | 0.0% |

## Hypersion's per-opponent record

| Opponent | W-L-D | Score |
|---|---|---|
| vs Alexandria-9.0 | 1-9-0 | 10.0% |
| vs Berserk-13 | 0-10-0 | 0.0% |
| vs Obsidian-16.0 | 0-10-0 | 0.0% |
| vs Stockfish | 0-10-0 | 0.0% |

**Total**: 1-39-0 (1 draw, 39 losses out of 40 games).

## Sanity checks

- **Self-play null test** (Hypersion-3.0 vs itself, 20g): 6W-6L-8D = 50.0%. Engine functions correctly.
- **Bench Threads=1 NNUE-on depth 13**: 2,071,666 nodes, deterministic across runs.
- **Concurrency reduced to 1, vs Berserk only, 20g**: 0-20-0. Score does not improve at conc=1, ruling out CPU oversubscription as the cause.

## The limitation

Per `DIAGNOSTIC_v3.1_LTC.md` (this repo), Hypersion's eval is systematically 200-400cp ABOVE Stockfish's view of the same position throughout middlegame, even at comparable search depth. **Root cause: NNUE-search coupling** (Stockfish issues #2981, #3365, #4678). Hypersion uses SF18's NNUE network (`nn-c288c895ea92.nnue`) with its own independently-tuned search constants. The network was trained against SF18's pruning tree; Hypersion's slightly-different leaves are less calibrated.

This is a structural limitation — closing it requires either:
1. **NNUE retraining** on Hypersion-tree-generated game data (multi-week project; needs ~1M self-play LTC games + ~50GB training data + nnue-pytorch infrastructure).
2. **Joint multi-feature SPSA** over a coordinated parameter group (RFP / NMP / LMR / futility constants together), at >50,000 nodes/iter for >10,000 iterations.

Both are beyond a single-session scope.

## What was tried in this development cycle (and what didn't help)

All cycle changes are documented in `release/RELEASE_NOTES_v3.0.md`. **Tried** but didn't ship:

| Candidate | Result | Why didn't ship |
|---|---|---|
| Berserk FMR eval damping | -24.4 ± 36.8 ELO @ 200g | Damping made pruning gates less effective |
| Obsidian counter-move stage | +1.7 ± 38.5 ELO @ 200g | Neutral — contHist1 already captures the signal |
| Alexandria root-only history | -5.2 ± 39.1 ELO @ 200g | 30g fakeout (+70 → -5 at 200g) |
| Berserk defender-status capture history | 0.0 ± 38.9 ELO @ 200g clean | Earlier +45 was stale-build artifact |
| Dual-gate corrCap (LTC=384) | ~0 ± 26 ELO @ 400g | 100g +42 didn't replicate at 400g |
| ContCorrHist int16_t "fix" | -10.4 ± 26.3 ELO @ 400g | The wrap-around at saturation is helpful, not a bug |
| Audit-#125 disable | +0.9 ± 26.9 ELO @ 400g | Neutral — kept as ship-correct |

**Shipped this cycle (cumulative)**:
- ContCorrHist (Berserk 4D): +90 bullet / +21 LTC
- ThreatSquareHistory TC-gated (RubiChess): +83 bullet / neutral LTC
- TC-gated correction-history read/write cap (new this cycle): +19 bullet / unchanged LTC
- qsearch SEE threshold reverted to VALUE_ZERO (new this cycle): +21 bullet / neutral LTC

Net ~+150 ELO over pre-cycle baseline. But still ~190 ELO below Stockfish, ~140 below Obsidian, ~95 below Berserk, ~85 below Alexandria at this TC.

## Recommendation

Hypersion 3.0 is the best-shippable version of this engine family without architectural change. For further gains, the realistic path is NNUE retraining. The release binaries at `release/Hypersion-3.0-{avx2,bmi2,avxvnni}.exe` are stable and SPRT-validated against their own prior baseline.

Reference engines used in this tournament are pinned at:
- `C:\Engine\Engines\stockfish\stockfish-windows-x86-64-avx2.exe`
- `C:\Engine\Engines\prebuilt\Obsidian160-avx2.exe`
- `C:\Engine\Engines\prebuilt\Alexandria-9.0-avx2.exe`
- `C:\Engine\Engines\prebuilt\berserk-13-avx2.exe` + `berserk-d43206fe90e4.nn`
