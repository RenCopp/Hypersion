# NPS optimization — what works and what doesn't

## Hypersion vs Stockfish NPS gap

At Threads=1, conc=2:
- Stockfish 18: ~1.5-2M NPS
- Hypersion BASE: ~600-800K NPS
- Hypersion + L1simd: ~700-900K NPS (commit 3e69bcb, +23 ELO)

Gap ≈ 2x. Not all of this can be closed by code (SF's NNUE training is
much bigger; their network is 102MB vs Hypersion's 102MB — same size, but
SF has more eval-table tricks).

## What's already optimized

- **L1 transform**: AVX2 SIMD (commit 3e69bcb).
- **FT incremental update**: SIMD via `simd_acc_add_i16`/`simd_acc_sub_i16`.
- **FT refresh**: full re-evaluate from king square; uses Finny cache.
- **FC0/FC1/FC2**: per-row `simd_dot_i8_u8` with `dpbusd` if VNNI else
  `maddubs+madd`.
- **Move generation**: magic bitboards (standard).
- **Repetition detection**: hash-key compare with min(rule50, pliesFromNull) walk.
- **TT prefetch**: at `do_move` for child position's key.
- **PawnCorrHist prefetch**: same.
- **Killer/counter-move tables**: tight in-cache.

## What was tried and failed

| Optimization | Result |
|---|---|
| PGO + AVX-VNNI | -45 ELO @ conc=6/61g (cache-masking suspected) |
| Eval cache (16K thread-local) | -35 @ conc=6/70g (cache-masking suspected) |
| Scrambled FC weight layout | +5.2 ± 37.9 @ conc=2/200g (neutral) |
| Float→int operation reorderings | tested in nnue.cpp, no visible win |

## What hasn't been tried

- **AVX-512** build (-mavx512f). Hypersion's host has AVX2 only currently.
- **Fused FT+L1 transform** in a single pass (avoid intermediate buffer).
- **Lazy NNUE** (skip NNUE for nodes deep in cut-line, use cheaper eval).
  Risky for ELO. Probably not net-positive.
- **NNUE quantization-aware approximations** (e.g. piecewise-linear ReLU
  with int16 input). Would need network re-export.
- **WDL output head** (combined with cp head; SF18's score model). Eval-side
  win, NOT NPS — different category.

## Profiling tools

```
# Build with -pg for gprof:
make CXXFLAGS_EXTRA="-pg" -j

# Run a fixed search and profile:
./Hypersion bench 16 > /dev/null
gprof Hypersion gmon.out | head -50

# Or use perf on Linux (Hypersion's host is Windows; works in WSL):
perf stat -e cache-misses,cycles,instructions ./Hypersion bench 13
perf record ./Hypersion bench 16
perf report
```

On Windows, can use VTune or Visual Studio profiler. Less convenient.

## Key cache concerns

- **TT (16-64MB)**: hits L3 only. Random-access pattern; prefetch helps.
- **NNUE FT weights (102MB)**: too big for L3 (typical 16-32MB). Streamed
  during evaluate(). Sequential access pattern is cache-friendly.
- **Histories (~10-20MB total per thread)**: fits in L2/L3. Random access
  by [piece][to][victim] etc.
- **Position state (~1KB)**: fits in L1. Hot.

**Cache-line padding** can hurt: if a struct grows past 64 bytes due to
new fields, accesses split across two cache lines. Profile before adding
state to Worker / Position.

## Hypersion bench non-determinism

Open bug. Empirically:
- Same binary, same input, depth=12 startpos: 124K..224K nodes across runs.
- SF18 same setup: deterministic.
- std::sort → stable_sort regressed -56 ELO (per-call overhead).
- Eval is byte-equivalent across runs (`testing/eval_diff.py` proves).
- Likely root cause: TT atomic-write-on-read pattern (line 44 in tt.cpp
  writes `genBound8` during probe), or memory-ordering effect from
  `nodes.fetch_add(relaxed)` interaction with cache state.

**Implication for testing**: don't trust single-run bench for NPS deltas.
Use 5+ runs, take median. Or use SPRT (which is unbiased even when
non-deterministic, since both sides see equal noise).

## Heuristic for "is this optimization worth trying"

- **Eval-side change**: needs +5-10 ELO at 200g. Easier to test (less noise).
- **Search-side change**: needs +5-10 ELO at 200g. Cache-friendly tweaks
  most likely to win.
- **Memory-aggressive change** (NNUE, weight layout, cache infra): test at
  conc=2 (60g+ if positive trends, 200g for confirmation). Easy to be
  fooled at conc=6.
- **Pure NPS optimization** (compiler flags, intrinsic swaps): only ships
  if it survives a 200g SPRT — NPS+ELO correlation isn't 1:1.
