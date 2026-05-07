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
deterministic; Hypersion's is not (root cause unidentified, likely TT
or memory-ordering related).

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
