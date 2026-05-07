# ELO testing theory — applied to Hypersion

## SPRT basics

SPRT (Sequential Probability Ratio Test) terminates as soon as the data
crosses one of two log-likelihood-ratio (LLR) bounds:
- LLR ≥ A = log((1-β)/α) → accept H1 (effect ≥ elo1)
- LLR ≤ B = log(β/(1-α)) → accept H0 (effect ≤ elo0)

Standard config: α = β = 0.05, elo0 = 0, elo1 = 5.
- A ≈ +2.94, B ≈ -2.94
- Expected sample size at "true ELO = +2.5": 1500-3000 games.
- At "true ELO = +5": 600-1500.
- At "true ELO = 0" (true neg): 600-1200.

Per Hypersion's PROTOCOL.md we use **fixed-game matches** (not SPRT) at
30g/200g checkpoints for triage speed. This trades type-I/II error
guarantees for predictable wallclock — but you still get an ELO ± CI from
each match.

## Confidence interval interpretation

After N games with score (W, D, L):
- Score ratio: `s = (W + D/2) / N`
- ELO point estimate: `400 · log10(s / (1-s))`
- CI half-width approx: `1.96 · sqrt(s·(1-s)/N + draw_adj)` — converted
  to ELO via the logit transform.

Rough table for 5+0.05 conc=6:
| N | typical CI half-width |
|---|---|
| 30  | ±100..±150 ELO |
| 60  | ±60..±90 |
| 100 | ±50..±60 |
| 200 | ±35..±40 |
| 500 | ±22..±26 |
| 1000 | ±16..±18 |

**This means:** to confirm a +5 ELO improvement with 95% confidence, you
need ~1000 games minimum. That's why 200g + tight tolerance ([+5, +10]) is
the project's cutoff — accepts as small as +5 with margin for error.

## The 30g fakeout pattern (PROTOCOL.md)

At 30g, ±100 ELO CI means a TRUE neutral/slightly-negative change has a
~1-in-3 chance of showing as "+50 ELO". Hypersion's session log has many:
- nmpR5: +107 @ 30g → +0 @ 200g
- lmrDiv12k: +70 @ 30g → -14 @ 200g
- razPD480: +47 @ 30g → -52 @ 61g
- cont3: +47 @ 30g → -7 @ 200g

**Rule**: 30g positives in [+5, +50] are noise. Always confirm at 200g.

## Concurrency masking (cutechess #630)

Cutechess at concurrency=6 runs 12 engines simultaneously, all sharing L2/L3
cache. Memory-aggressive optimizations (NNUE SIMD, weight layout, PGO) get
their wins ERASED by cache contention. Empirical:

| Optimization | conc=6 | conc=2 |
|---|---|---|
| L1-transform SIMD | +0.0 ± 38 | +23.2 ± 65 |
| PGO + AVX-VNNI | -45 @ 61g | (positive expected, not yet rerun) |
| Eval cache (16K) | -35 @ 70g | (positive expected, not yet rerun) |

**Rule**: For NNUE/cache changes, test at conc=2. For pure search-logic,
conc=6 is fine.

## Gauntlet format (round-robin)

If testing multiple candidates against same baseline, prefer **gauntlet**
(each candidate plays baseline N times, candidates don't play each other).
Saves time vs full round-robin. cutechess: `-rounds N -games 2 -repeat`.

## Time control choices

| TC | Use case |
|---|---|
| 5+0.05 | Fast triage, conc=6. Cheap. Most measurements here. |
| 10+0.1 | Slower triage. Better signal/cost than 5+0.05? Not validated. |
| 60+0.6 | Long-TC validation. Run every 4-6 ships to confirm gains hold. |
| 30+0.3 | vs Stockfish-with-Elo-limit. For blunder analysis. |

**Long-TC validation is critical.** Search depth scales with time, and
some heuristics that win at 5+0.05 lose at 60+0.6 (over-pruning at low
depth, or vice versa). Hypersion has had +5 @ 5s short-TC ships that
were -8.7 @ long TC.

## Sources of ELO noise

1. **Cutechess timing jitter** — at fast TC, kernel scheduling matters.
   Always close other apps; use `-recover` flag.
2. **Opening book** — different openings give different draw rates.
   Use same EPD across all tests (popularpos_lichess_v3.epd).
3. **Match length variance** — shorter games = wider draw rates and noisy
   ELO. Avoid TC < 5+0.05 unless really triage-only.
4. **Engine non-determinism** (Hypersion bug) — same engine same opening
   plays differently, which adds noise to all measurements but doesn't
   bias them (both sides are equally affected).
