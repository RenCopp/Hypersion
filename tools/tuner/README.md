# Hypersion Texel Tuner

Status: **scaffold**. Loads labeled positions and reports MSE; doesn't yet
mutate eval constants. The remaining pieces (data extraction, parameter
mutation) are documented below.

## Quick start

```
make tuner ARCH=x86-64-avx2
tuner --in tuned_positions.txt
```

Output:

```
loaded 50000 labeled positions from tuned_positions.txt
baseline: K=1.05  MSE=0.108234
```

Lower MSE = the eval predicts game outcomes better.

## Data file format

One record per line:

```
<fen> | <result>
```

where `<result>` is `1` (white won), `0` (black won), or `0.5` (drew).

Example:
```
rnbqkb1r/pppp1ppp/5n2/4p3/4P3/2N5/PPPP1PPP/R1BQKBNR w KQkq - 0 3 | 0.5
```

## How to get data

The Texel approach needs **millions** of labeled positions. Three viable
sources:

### Option A — self-play with cutechess (slowest, best quality)
Run cutechess matches at very fast TC, save the PGNs, sample positions:
```
cutechess-cli -engine cmd=Hypersion.exe -engine cmd=Hypersion.exe \
  -each tc=1+0.01 -games 10000 -pgnout selfplay.pgn
```
Then walk the PGNs (TODO: write `pgn_to_positions` — see below).

### Option B — Lichess open database (fastest)
[database.lichess.org](https://database.lichess.org/) publishes monthly PGN
dumps with millions of games. One month is ~30 GB compressed → hundreds of
millions of positions after sampling.

### Option C — re-use the 12.8 GB `2025 Database.pgn` already on disk
Same approach: walk the PGN, sample every Nth ply, label with game result.

All three need the missing `pgn_to_positions` C++ tool — a SAN walker that
applies each move via Hypersion's movegen, samples positions, and emits
`(fen | result)` records. ~250 lines of C++. Mostly ports of the deleted
`tools/pgn_extract` (which was custom for HNNU and got cleaned up).

## Steps left to implement full Texel tuning

### 1. Extract data (~250 lines C++)

`tools/tuner/pgn_to_positions.cpp`:
- Streams the PGN
- Parses headers, gets `[Result "X"]`
- Parses the move text in SAN (the hard part — disambiguators, captures, promotions, castling)
- Uses `Position::do_move` to walk the game; resolves SAN against `MoveList<LEGAL>`
- Samples one position every 4–6 plies (optional `--skip-opening 8 --skip-tail 6`)
- Emits `<fen> | <result>` lines

### 2. Make eval constants tunable at runtime (~50 lines refactor)

In `src/evaluate.cpp`, move the tunable `constexpr int` constants into a
`struct EvalParams { ... };` defined in a new `src/eval_params.h`:

```cpp
// src/eval_params.h
namespace hypersion::Eval {
struct Params {
    int IsolatedPawnPenalty = 13;
    int DoubledPawnPenalty  = 18;
    int BackwardPawnPenalty = 9;
    int BishopPairBonusMG   = 30;
    int BishopPairBonusEG   = 50;
    int RookOpenFileMG      = 25;
    /* ... all the rest ... */
};
extern Params params;
}
```

Then `evaluate.cpp` reads from `params.IsolatedPawnPenalty` etc. The struct
is mutable at runtime so the tuner can twiddle it.

For PSQT and other arrays: same idea, just store them in `params`.

### 3. Coordinate-descent loop (~100 lines, replaces the stub in `tuner.cpp`)

```
for each parameter p in params:
    for delta in {-2, -1, +1, +2}:
        old = p
        p = p + delta
        new_mse = mse(records, K)
        if new_mse < best_mse:
            best_mse = new_mse
            keep change
        else:
            p = old
repeat until no parameter changes in a full sweep
```

Stockfish's tuner does fancier gradient descent with momentum, but the
coordinate-descent version above is what most engines use and is what
Texel itself used (hence the name).

## Expected gain

Empirically, a first Texel pass over Hypersion's ~30 tunable constants on
~5 M positions typically yields **+30 to +80 ELO**, mostly by replacing
guesses with data-fit values.

After the first pass, subsequent passes (and adding new tunable terms)
yield diminishing returns — usually +5–15 ELO per pass.
