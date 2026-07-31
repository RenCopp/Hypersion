# Hypersion Testing Protocol

## SPRT Triage / Confirm

All proposed engine changes go through a two-stage validation:

### Stage 1 — 30-game triage
- Concurrency: 6, TC 5+0.05, openings: popularpos_lichess_v3.epd
- Outcome ≤ -50 ELO  → **REJECT**, tombstone in source
- Outcome in [-50, +50] → **NOISE**, skip unless theory predicts win
- Outcome > +50  → **PROMISING**, run Stage 2

### Stage 2 — 200-game confirmation
- Same TC, concurrency 6
- Outcome ≤ +5 with CI ±35 → **REJECT**, tombstone
- Outcome > +10 → **SHIP**

### Stage 3 — Long-TC validation (every 4–6 ships)
- TC 60+0.6, 200 games
- Confirms that the change is not TC-specific

## 30g Fakeout Pattern

This session has documented many cases where 30g positives regressed at 200g:
- nmpR5: +107 @ 30g  → +0  @ 200g
- lmrDiv12k: +70 @ 30g → -14 @ 200g
- razPD480: +47 @ 30g → -52 @ 61g (aborted)
- cont3: +47 @ 30g → -7 @ 200g

**Rule**: 30g positives in [+5, +50] are noise. Always confirm at 200g.

## Tactical Suites (when EPDs available)
- WAC (300 positions, depth 18)
- BT2630 (30, depth 22)
- ECM (879, depth 12)
- STS (1500, depth 12)

## Bench Signature
- `Hypersion bench` runs 8 fixed positions at depth 13.
- **Bench is deterministic with explicit `setoption name Threads value 1`.**
  Lazy-SMP searches can traverse different trees, so multi-thread node totals
  are not benchmark signatures. The current clean-build depth-13 signature is
  stored in `BENCH_SIGNATURE` and read by the smoke, regression, and Makefile
  verification gates.
- Use this incantation for deterministic bench:
  ```
  uci
  setoption name Threads value 1
  bench 13
  ```
- For NPS comparisons during development, the `setoption Threads 1` prefix
  also gives stable measurement — earlier "±28% variance" was lazy-SMP race,
  not a bug. NPS does still vary a few % from OS scheduling jitter; take
  the median of 5+ runs.
- A bench-signature change tied to search-semantics justification is fine;
  don't try to use bench for byte-exact equivalence checks.

## UCI lifecycle safety

- Run `test_uci_lifecycle.py` before publishing changes to UCI options, search
  startup/shutdown, TT, NNUE, Syzygy, or thread-pool ownership.
- Resource-mutating options must stop and join active workers before replacing
  or clearing shared state.
- EOF is a shutdown request: an active infinite or ponder search must be stopped
  and joined instead of leaving an orphaned engine process.
- Every accepted `go` must produce exactly one legal `bestmove`, including when
  interrupted by reconfiguration, EOF, a second search, or shutdown.
- Spin options must enforce the same bounds advertised by `uci`; malformed or
  out-of-range values must not create oversized pools or invalid time settings.
- Untrusted FEN must be structurally validated before it reaches the optimized
  board parser. Histories beyond `MAX_GAME_PLIES` must be compacted without an
  out-of-bounds `StateInfo` write.
- The bounded test is a fast regression gate, not a substitute for sanitizer
  builds and the longer mixed-command soak required before raising the default
  above `Threads=1`.

## Concurrency for Memory-Aggressive Optimizations

The default `--concurrency 6` SPRT setting masks the ELO of memory-aggressive
optimizations (PGO, eval cache, NNUE SIMD reorderings, weight-layout changes)
because 12 simultaneous engine threads compete for shared L2/L3 cache. This
is a known cutechess problem ([cutechess #630](https://github.com/cutechess/cutechess/issues/630));
[OpenBench](https://github.com/AndyGrant/OpenBench) avoids it by spawning
multiple cutechess processes at lower per-process concurrency.

**Empirical verification on Hypersion**:

| Optimization | concurrency=6 (200g) | concurrency=2 (60g/200g) |
|---|---|---|
| L1-transform SIMD (commit 3e69bcb) | **+0.0 ± 37.7 ELO** | **+23.2 ± 64.9 ELO** |
| AVX-VNNI build (`ARCH=x86-64-avxvnni`) | -45 @ 61g | **+58.5 @ 60g, +29.6 ± 35.6 @ 200g** (SHIP) |
| Eval cache (thread-local 16K) | -35 @ 70g | (expected positive, not yet rerun) |
| SF-scrambled FC0+FC1 layout | (not run at conc=6) | **+5.2 ± 37.9 @ 200g** (REJECT, neutral) |
| Game-workload PGO (30 self-play games training) | -88 @ 60g (bench-PGO) | +107.5 @ 30g, **-40.1 ± 38.0 @ 200g** (REJECT, fakeout); re-test post A2-v2/A3 ship: -6.9 ± 37.1 @ 200g (TOMBSTONE confirmed) |

**Rule**: For memory-aggressive optimizations (NNUE SIMD, weight layout, PGO, cache
infrastructure), test at `--concurrency 2`. For pure search-logic changes,
`--concurrency 6` is fine and gives more games per wall-clock minute.

The 200g target also adjusts: at concurrency=2, 60g matches give cleaner signal
than 200g at concurrency=6 because each game is uncorrupted by inter-process
cache contention.

## Tombstone Convention

Negative results are recorded as inline comments at the affected source location, not deleted. Format:

```cpp
// NOTE: tested X (e.g. SF18 PawnHistory port). Result tombstone:
//   variant A: -49 ELO @ 100g (clear regression)
//   variant B: -23 ELO @ 30g (within noise but trending bad)
// Reason: ...
// Future contributor wanting to re-attempt should ...
```
