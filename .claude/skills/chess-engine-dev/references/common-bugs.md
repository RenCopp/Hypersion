# Common chess engine bugs — diagnostic playbook

## Eval drift / inconsistent eval

**Symptom**: same FEN gives different `eval` outputs across runs, or eval
differs after `position fen X` vs play-to-X via moves.

**Causes**:
1. Incremental accumulator desync — accumulator differs from full refresh.
   *Fix*: log accumulator hash at key points, verify equality.
2. NNUE quantization saturation — int8 multiply can saturate when input is
   close to ±127. Causes drift visible only in extreme positions.
   *Fix*: use higher-precision intermediate (int16 accumulator) for FC layers.
3. Padded-input SIMD missing tail bytes — see Hypersion's FC1 fix
   (passing pi=30 vs padded=32). The unread bytes weren't zero in some path.
4. Endianness or alignment in serialized weights — only matters cross-platform.

**Verification**: write an eval-diff harness — feed N positions to two
engines, compare every `static eval` value. Hypersion's
`testing/eval_diff.py` does this. 200/200 match = byte-equivalent.

## NPS regression with no apparent code change

**Causes**:
1. **Compiler reordering** with `-flto` — small source changes can rearrange
   hot loops. Compare `objdump -d` of relevant sections.
2. **Cache contention** — adding a member to a struct can push a hot field
   off a cache line. Profile L1/L2 miss rates.
3. **Alignment changes** — `alignas(64)` on a struct may pad more than
   expected and shift other things.
4. **Linker order changes** — `-flto` can change function placement; bench
   warm-up may hit different L1i state.

**Diagnostic**: bench at Threads=1 with 5+ runs, take median. Don't trust
single readings. Hypersion bench varies ±25-40% across processes
(open bug); use SPRT for ELO-grade NPS validation.

## Threading non-determinism

**Symptom**: same input → different bestmoves with multiple threads.

**Expected behavior**: Lazy SMP IS non-deterministic by design. With Threads=N
> 1, the search tree differs across runs because helpers race. Don't try to
make it deterministic — just use SPRT (which is unbiased).

**Bug if**: Threads=1 also produces different results. SF Threads=1 is
deterministic; Hypersion's at explicit `setoption name Threads value 1` IS
deterministic too (verified 2026-05-07 — same 1,105,922 bench nodes across
5 runs at depth 13). Earlier claim of "Threads=1 non-determinism" was based
on bench being run WITHOUT explicit Threads=1, which inherited the default
Threads=2 (lazy SMP, non-deterministic by design).

## TT corruption / wrong-bestmove from TT

**Symptoms**: bestmove suddenly changes deeply into iter-deepening; eval
flips wildly between iterations.

**Causes**:
1. Concurrent writes to same TT entry without atomic. SF uses XOR-key
   protection (`key XOR data` stored, validated on probe).
2. Hash-key collision (different positions, same Zobrist key low bits).
   Should be ~1 in 2^48 with 64-bit keys; usually negligible but
   can corrupt repetition detection.
3. Aging bug — old entries from previous game leaking. Check
   `TT.new_search()` increments generation correctly.

## Repetition / draw-by-rule bugs

**Symptom**: engine loses winning position by drifting into 3-fold rep, or
declines a forced draw it could claim.

**Causes**:
1. `pliesFromNull` not preserved across position->set() — repetition walk-back
   uses min(rule50, pliesFromNull) so 0 there means no walk. Hypersion fixed
   this in `Worker::prepare`.
2. `is_draw()` in qsearch not checking ply, so qsearch sees draw too eagerly.
3. Side-to-move sign error in repetition score — should be -contempt for
   opponent's preferred draw, +contempt for ours.

## Time management bugs

**Symptoms**:
1. Engine flags out (loses on time) — timeman returning too aggressive.
2. Engine moves instantly without searching — TT-move-without-search bug.
3. Engine plays "panic" non-progressing moves at low TC (Hypersion's
   known bullet bug with passed pawn).

**Diagnostic**: log `optimum`/`maximum` from timeman, log `tm.elapsed()`
in iter-deepening, look for the iteration where the budget was exceeded.

## Move generation bugs

**Symptom**: position becomes illegal after a sequence of moves, perft
mismatches Stockfish.

**Diagnostic**: `perft 5` should match Stockfish's perft 5 EXACTLY at every
depth from 1 to 5. Hypersion has `cmd_perft`. If mismatch, divide-and-conquer
by FEN to isolate the bug.

## NNUE accumulator desync

**Symptom**: eval after `do_move + undo_move` differs from eval before.

**Diagnostic**: add a debug check at top of evaluate() that compares
incrementally-maintained accumulator to a fresh-refresh accumulator. If
they differ, the incremental path has a bug — usually in king-move handling
(king move triggers full refresh, which interacts with finny cache).

## Search instability without code change

**Cause**: TT clearing not happening between bench positions, or between
ucinewgame events. Check: log `TT.hashfull()` at start of each bench position
— should be 0 (just after clear()).

## Tools ready for use

- `testing/vs_stockfish.py` — match Hypersion vs Stockfish, classifies blunders.
- `testing/analyze_with_sf.py` — per-move SF eval comparison from PGN.
- `testing/eval_diff.py` — byte-equivalence check across positions.
- `testing/sprt.py` — proper SPRT runner.
- `Hypersion perft N` — move generation correctness.
- `Hypersion eval` — single-position eval print.

## Anti-pattern: "give first own search more resources"

Tombstones in `src/timeman.cpp` (v8 H1 + v16) and `src/search.cpp`
(v13) document THREE independent attempts to perturb the first 1-3
own-search moves per game (where the TT is cold). All three regressed
at TC 5+0.05 conc=6 200g — in BOTH directions:

| Attempt | Mechanism | SPRT 200g result |
|---|---|---:|
| v8 H1 | +30 % optimum-time budget for moves 1-3                | -24.4 +/- 39.3 ELO |
| v13   | 4x wider initial aspiration window for first own move | -10.4 +/- 38.3 ELO |
| v16   | -23 % optimum-time budget for moves 1-3 (inverse of H1)| -15.6 +/- 38.2 ELO |

**Lesson**: the first-move time/window settings are at a TIGHT local
optimum at TC 5+0.05. NEITHER more search effort (v8 H1 / v13) NOR
less (v16) helps. The original underlying tunings (ASPIRATION_DELTA0
= 50, the time-budget formula in timeman.cpp) are jointly calibrated
across the existing parameter set.

v8 H1 and v13 (more-effort variants) also showed a Black-side
asymmetry: candidate-as-Black scored ~39 % while candidate-as-White
scored ~57 %. The deeper / more reliable first-move eval reveals
Black's structural disadvantage in typical EPD openings too clearly,
causing the engine to commit to passive defense that gives up
initiative over the rest of the game. v16 (less effort) had no
Black asymmetry — both sides under-performed by ~3-5 ELO instead.

If you find yourself reasoning "the first own search has an empty TT,
so [more time / wider window / pre-warming] should help":

1. **Re-read the BLUNDER_ANALYSIS.md observation**: the actual bullet
   game blunders cluster at moves 8-30, NOT in the first 3 own moves.
   The "empty TT" intuition targets the wrong window.
2. **Test color-asymmetric variants instead** — if the failure mode is
   Black-side, gate the effect on `rootPos.side_to_move() == WHITE`.
3. **Don't ship without confirming Black asymmetry is gone** — split
   SPRT result by NEW-as-White vs NEW-as-Black; if Black is below 45 %,
   reject regardless of net score.

The `SearchLimits::ownSearchIndex` field and the uci.cpp counter (added
during v8 H1, repurposed by v13) remain in tree for future variants.

## Anti-pattern: "interior sweep point" SPSA-mimicking experiments

Session 2026-05-15 tested 7 interior sweep points across the existing
SPSA-tuned parameter set (LMR divisor, RAZOR/SEE/FUTIL margins, HIST
weights). Only ONE was a real ship; SIX were 30g-fakeout REJECTS.

| Attempt | Param | Change | 30g triage | 200g result |
|---|---|---|---:|---:|
| v18 | LMR_DIVISOR        | 1.85 -> 1.87 | +23.2 | **+20.9 SHIPPED** |
| v22 | RAZOR_MARGIN_BASE  |  852 -> 750  | +11.6 | -8.7 REJECTED |
| v23 | SEE_QUIET_MARGIN   | -181 -> -200 | +23.2 | -15.6 REJECTED |
| v25 | FUTIL_PER_DEPTH    |  397 -> 410  | -58.5 | (skipped, ≤-50 at triage) |
| v26 | HIST_BONUS_CAP     | 2065 -> 2080 | +58.5 | +1.7 REJECTED |
| v28 | CONT2_WEIGHT       |   48 -> 49   | +46.6 | +1.7 REJECTED |
| v29 | HIST_BONUS_DEPTH1  |   30 -> 31   | +58.5 |  0.0 REJECTED |

The only successful attempt (v18) had a 30g triage RIGHT at the +50
protocol threshold (+23). All six attempts with 30g >+30 reverted to
near-zero at 200g — confirming the **"30g [+5, +50] fakeout" anti-
pattern** documented in CLAUDE.md / PROTOCOL.md.

**Rule of thumb for SPSA-tuned param sweeps**:
- 30g triage > +50 with opening-set noise CI ±100 looks great but
  is HIGHLY susceptible to opening selection (cutechess sequential
  order draws from the same EPD prefix per match, but small samples
  produce ~70+ ELO swings just from which side of the matchup gets
  the better opening repertoire first).
- Stage 2 200g tightens CI to ±35-40 and almost always exposes the
  fakeout.
- The interior sweep only works when BOTH endpoints of the SPSA-
  documented range show positive results vs the prior baseline.
  LMR 1.85 vs 1.95 had clear interior optimum; SEE_QUIET -150 (-70 ELO)
  vs -220 (-1.7) has no positive interior to find.

**Recommended workflow**:
1. Inspect the inline sweep comments in src/search.cpp for the
   parameter's known endpoint results.
2. Only test interior points when both endpoints are positive
   relative to their reference baseline.
3. Be ready to tombstone 5 of 6 attempts even when the parameter
   class is well-chosen.

This is why v18 was a real ELO ship and most other SPSA-mimicking
attempts in this engine don't move the needle. The remaining ELO
ceiling for isolated parameter tweaks is exhausted; future progress
needs joint multi-parameter SPSA campaigns or NNUE retrain.
