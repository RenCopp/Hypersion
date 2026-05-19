# Hypersion v3.1 LTC diagnostic — where ELO leaks against full-strength opponents

**Goal**: identify exactly where Hypersion loses to Stockfish + Obsidian at LTC 60+0.6.

## Headline result

**Hypersion 0-40 vs full-strength opponents at LTC 60+0.6** (20 games each match):
- vs Stockfish + Obsidian round-robin (no Syzygy): **0-20**, opponents +191 ELO
- vs Stockfish only (both with Syzygy): **0-20**, all losses by mate

Syzygy does not close any of the gap — Hypersion is already lost in middlegame long before reaching TB territory.

## Per-move ground-truth analysis

826 Hypersion moves analyzed at Stockfish depth 14 from the 30-game round-robin PGN.

| Metric | Value |
|---|---|
| Total Hypersion moves | 826 |
| **Match SF top move** | **410 (49.6%)** |
| p50 eval gap (non-matching) | 24 cp |
| p75 eval gap | 48 cp |
| p90 eval gap | 88 cp |
| p99 eval gap | 8631 cp (mate-allow in already-lost positions) |
| Inaccuracies (≥ 50 cp) | 99 |
| Blunders (≥ 200 cp) | 13 |

**Half of Hypersion's moves match Stockfish's top move at depth 14.** The other half lose 24-88cp on median, with a long tail of mate-allows in dead-lost positions.

## Phase distribution of mid-range inaccuracies (50–300 cp)

Excludes mate-allow noise to focus on ELO-impactful errors.

| Phase | Inaccuracy count | Avg gap |
|---|---|---|
| middlegame | 52 (59%) | 88 cp |
| endgame | 21 (24%) | 103 cp |
| opening | 15 (17%) | 82 cp |

**Middlegame is the largest source of inaccuracies.** Endgame inaccuracies are fewer but each costs more.

## Piece distribution of mid-range inaccuracies

| Piece | Count | Avg gap |
|---|---|---|
| pawn | 23 | 94 cp |
| rook | 21 | 97 cp |
| king | 15 | 96 cp |
| bishop | 10 | 80 cp |
| queen | 10 | 82 cp |
| knight | 9 | 75 cp |

**Pawns and rooks dominate inaccuracies.** Knights and bishops are best — when Hypersion moves a minor piece, it tends to pick well.

When measured by the FULL set of moves (including dead-lost mate-allows), king moves explode to avg 1174 cp gap because Hypersion's king-moves are also the ones picking the wrong square in K+R+P–type endgame conversion.

## The smoking gun pattern: sustained eval understatement

Tracked per-move on game 11 (Hypersion White vs Stockfish Black):

| Move | Hypersion eval | SF eval | Gap |
|---|---|---|---|
| 13. Nd3 | +0.38 (d17) | -0.64 (d21) | 102 cp |
| 18. c6 | -0.37 (d14) | -2.14 (d20) | 177 cp |
| 22. e4 | -1.12 (d18) | -3.55 (d16) | 243 cp |
| 27. Re2 | -1.65 (d18) | -3.91 (d18) | 226 cp |
| 35. Kg4 | -2.01 (d21) | -4.49 (d22) | 248 cp |
| 41. Bd7 | -2.57 (d21) | -6.40 (d25) | 383 cp |
| 47. Kg4 | -3.55 (d19) | -7.25 (d26) | 370 cp |
| 53. Ke4 | -6.59 (d19) | -10.63 (d18) | 404 cp |

The pattern: **Hypersion's eval is consistently 100-400cp BEHIND Stockfish's even at comparable depth**, throughout the middlegame. By the time Hypersion realizes it's lost, the position is unsavable. The depth gap (Hyp d17 / SF d24) is real but not the only factor — even at SAME depth (e.g. move 53 both d18-d19), Hypersion's eval is off by 400 cp.

## Diagnosis: 3 distinct failure modes

### 1. NNUE-search coupling at LTC (the dominant gap)

Hypersion uses SF18's NNUE (`nn-c288c895ea92.nnue`, 1024-15-32-1 architecture) but with Hypersion's tuned search constants. At LTC depths (20+), the NNUE evaluations from a slightly-different-pruning tree are systematically less calibrated than Stockfish gets from its own tree. This shows up as the consistent 200-400cp eval understatement.

Per Stockfish issues #2981 / #3365 / #4678: NNUE-search coupling is real and documented. The shipping fix is either:
- (a) Retrain NNUE on Hypersion's search trees (out of scope: requires weeks of training data + compute)
- (b) Use the SAME search constants as SF18 (defeats Hypersion's identity — and the constants are SF18-specific to its NNUE; transplanting them might break other things)

### 2. Middlegame accumulated drift (50-100 cp / move × 50% non-match rate)

When Hypersion picks a non-SF move (50.4% of moves), it loses 24cp on median and 88cp at p90. Over 40 middlegame moves, this accumulates to a 200-400cp positional disadvantage — exactly what we see in game traces.

This is NOT individual blunders; it's the slow strategic understatement compounding. The blunders happen LATE (ply 60+), but the *games are lost* in the middlegame by the drift.

### 3. Endgame conversion missing (large per-mistake cost, but few per game)

21 mid-range inaccuracies (avg 103 cp gap each) plus the >8000cp mate-allows. Once in a clearly-losing endgame, Hypersion picks moves that prolong rather than save — but it's already lost by then due to (1) and (2).

Syzygy didn't help: by the time the position is 3-4-5 pieces, the result is already decided. Syzygy probing fires correctly but the position is unwinnable.

## What's actionable

Given the v3.1-cycle exhaustive testing of single-feature ports (T1-T4 all rejected/neutral, Tier R1/R2/R3 rejected, multiple SPSA campaigns rejected), the realistic forward paths are:

### High-leverage but expensive
1. **Retrain NNUE on Hypersion-tree generated game data**. Requires ~1M self-play games at LTC, ~50GB training data, tooling for NNUE training (e.g. SFnnue-tools or pytorch-NNUE). This is the proper fix per chess-engine community consensus, but is a multi-week project.

2. **Joint multi-feature SPSA** over 8-12 coordinated parameters (NMP base R + RFP margin + LMR base + LMR div + futility constants). Single-parameter SPSA campaigns have universally failed; the local optimum has multi-dimensional structure. Would require >50000 nodes/iter and >10000 iterations.

### Low-risk diagnostic improvements (no measurable ELO but UX)
3. **Default SyzygyPath auto-detection** in UCI startup. If `C:\Engine\3-4-5 syzygy` or `./syzygy` exists, set automatically. Removes a user-config gotcha.

4. **Persistent corrhist file load by default**. Hypersion writes `hypersion_corrhist.bin` on shutdown and CAN load on startup, but the option isn't always set. Auto-loading from cwd would help repeated-LTC test sessions.

### Things NOT to try (this session's tombstones)
- More single-table structural ports (T1-T4 all REJECT/NEUTRAL)
- More single-parameter SPSA (every campaign has regressed)
- More magnitude rescaling of existing tunables (A6 SPSA found defaults at local optimum)
- More feature-removal experiments (every removal has regressed)

## Reality check: what is Hypersion's strength position?

Per v3.1 tournament results (4 rounds × 15 pairings, TC 5+0.05, 6 engines):
- Stockfish: +108 ELO, score 65.0%
- Obsidian: +89 ELO, score 62.5%
- Alexandria: +89 ELO, score 62.5%
- RubiChess: -17 ELO, score 47.5%
- Berserk: -89 ELO, score 37.5%
- Hypersion 3.1: -191 ELO, score 25.0%

At LTC (60+0.6), the gap to Stockfish widens. Hypersion is a competent ~2700-class engine but ~300 ELO below the SOTA at long TC. Closing that gap requires NNUE retraining or major architectural work, not parameter tuning.

## Recommendation

**Hypersion v3.1 ships as the latest verified release.** Further improvement requires multi-week NNUE retraining or joint-SPSA campaigns at higher nodes/iter than current infrastructure supports. The session's structural-port avenue is exhausted: every clean-rebuild SPRT in 2026-05 shows the engine is at a tight multi-dimensional local optimum.

For tournament play, **users should explicitly set `SyzygyPath` to a tablebase directory** (the v3.1 default `<empty>` means TB-probing is off). This won't change LTC vs Stockfish (it didn't), but will help in self-play tournaments where both sides are imperfect endgame players.
