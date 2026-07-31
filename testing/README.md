# Testing scripts

Portable usage: pass local paths on the command line. `sprt.py` also accepts
the `CUTECHESS` and `HYPERSION_OPENINGS` environment variables. Generated PGNs
and logs remain ignored by Git.

The engine path defaults to the repository build for the current platform.
Use command-line options or the environment variables above for external tools
and opening files; scripts should not be edited for each machine.

## Fast correctness and lifecycle checks

```text
py testing/test_smoke.py --exe ./Hypersion.exe
py testing/regression.py --exe ./Hypersion.exe
py testing/test_uci_lifecycle.py --exe ./Hypersion.exe
py testing/test_smp_soak.py --exe ./Hypersion.exe --searches 1000
py testing/test_uci_fuzz.py --exe ./Hypersion.exe --cases 1000
py testing/test_thread_scaling.py --exe ./Hypersion.exe
make test_timeman
py testing/test_endgame_conversion.py ./Hypersion.exe --syzygy /path/to/syzygy
```

`test_uci_lifecycle.py` runs bounded multi-thread scenarios covering active
search interruption, hash clearing/resizing, NNUE and Syzygy option changes,
new-game resets, thread-pool resizing, EOF shutdown, fixed-seed mixed command
ordering, and clean shutdown. It uses classical fallback by default for speed;
add `--with-nnue` for a local NNUE-enabled pass and `--seed` to replay a mixed
sequence exactly. Every case also requires exactly one legal `bestmove` for
every `go` command and exercises out-of-range UCI options, malformed FEN input,
and move histories longer than the engine's fixed state buffer.

`test_smp_soak.py` holds each requested thread count fixed while replaying a
deterministic mix of depth, node, movetime, infinite, and ponder searches. The
full local gate uses `--threads 1 2 4 8 --searches 10000`; CI uses a smaller
count to keep build feedback fast.

`test_uci_fuzz.py` sends a fixed-seed bounded set of malformed FEN and unknown
commands, inserts readiness barriers, and then requires a valid recovery search.
`test_thread_scaling.py` measures median fixed-node NPS across Threads 1/2/4/8;
it can write a local JSON report with `--json-output`. The scaling report is a
small benchmark, not game telemetry. `make test_timeman` asserts exact optimum
and maximum budgets for movetime, overhead, low-clock, increment, ponder, and
non-clock search modes.

`test_endgame_conversion.py` requires `python-chess` and local Syzygy files. It
plays a small set of high-rule50 endings, records start DTZ and conversion data,
and distinguishes feasible wins from positions that cannot zero or mate before
the 50-move boundary.

For sanitizer instrumentation, run `make debug`. On Windows/MSYS2 this also
requires `mingw-w64-clang-x86_64-clang` and
`mingw-w64-clang-x86_64-compiler-rt`; the target
checks for the runtime before cleaning the existing release build.
The generated executable dynamically loads the ASan runtime, so add
`C:\msys64\clang64\bin` to `PATH` while running it. For example in PowerShell:

```powershell
$env:Path = 'C:\msys64\clang64\bin;' + $env:Path
$env:ASAN_OPTIONS = 'halt_on_error=1:abort_on_error=1:detect_leaks=0'
$env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
py testing/test_uci_lifecycle.py --exe ./Hypersion.exe --cycles 100
```

## Primary harness: `sprt.py`

The canonical way to validate any change. Wraps cutechess-cli with sane
defaults, parses the live output, prints a single-line progress meter,
and ends with a clean PASS / FAIL / INCONCLUSIVE verdict.

```
# Standard SPRT [0, 5] alpha=beta=0.05, 10+0.1, 4 concurrent games
py testing\sprt.py --new dist\Hypersion_candidate.exe ^
                    --old testing\Hypersion_baseline.exe

# Fixed-games match (no SPRT)
py testing\sprt.py --new ... --old ... --games 200 --no-sprt

# Sanity null test (same exe vs itself; should not pass H1)
py testing\sprt.py --new dist\Hypersion.exe --null
```

Use `--new-threads` and `--old-threads` to compare lazy-SMP settings while
keeping both engine binaries and all other options identical.

Exit codes: `0`=PASS (H1 accepted), `1`=FAIL (H0 accepted),
`2`=INCONCLUSIVE (game cap hit), `3`=process error.

### Standard SPRT bounds

| Use case          | elo0 | elo1 | TC      | Notes                            |
|-------------------|------|------|---------|----------------------------------|
| **Standard test** | 0    | 5    | 10+0.1  | First-line gate for all changes  |
| Strict regression | -3   | 1    | 10+0.1  | Used when investigating a regression |
| Long-TC validation| 0    | 5    | 60+0.6  | Final confirm after 10+0.1 PASS  |

## Legacy batch scripts

| Script | Purpose |
|---|---|
| `ab_match.bat [LABEL]` | Hypersion vs `Hypersion_v0.exe` (drop a baseline binary in this folder), 60+0.6 × 20 games. Useful for measuring incremental change. |
| `gauntlet.bat` | Round-robin gauntlet against multiple opponents (Stockfish, others). |
| `sprt_vs_stockfish.bat` | Sequential Probability Ratio Test vs Stockfish, autostops on bound. |

## Requirements

- [Cute Chess](https://cutechess.com/) — `cutechess-cli` binary
- Opening book in EPD or PGN format (e.g. ECO from the Cute Chess source tree)
- For `gauntlet.bat` / `sprt_vs_stockfish.bat`: the opponent engines

## Output

Each script writes a `.pgn` next to itself with the games. Use
[ordo](https://github.com/michiguel/Ordo) or the `bayeselo` tool to turn
the PGN into ELO numbers, or just import it into Cute Chess GUI to
browse the games.

## Notes

- Match scripts default to `Threads=1`. The full lazy-SMP sanitizer and
  10,000-search-per-count long-soak gate passes at Threads 1, 2, 4, and 8, but
  lifecycle stability alone does not establish multi-thread playing strength or
  wall-clock scaling.
- `Hash=64` MB is enough for 10+0.1; raise to 256 MB for slow TC.
- `concurrency=2` works on 4-core+ CPUs; raise to match your physical-core count.

## Tuning support scripts

### `wac_runner.py` — tactical suite at fixed depth

```
py testing\wac_runner.py --depth 8 --no-nnue --quiet
```

Runs the 198-position WAC suite at depth 8 with classical eval only
(`--no-nnue` unloads the network). Used to verify Texel-tuned values
don't regress tactical play.

Threshold: WAC >= 178/198 indicates the tune is safe. Session best
(2026-05-14): 184/198 (92.9%) with R20-R31 features active. Each run
takes ~3-5 min; if stuck for >10 min, kill — likely a stale-obj-file
hang requiring `make clean && make` (see `tuner/README.md`).

### `apply_tuned_values.py` — auto-apply tuner output

```
py testing\apply_tuned_values.py testing\tuner_run_output.txt
```

Parses the `=== TUNED VALUES ===` block from a tuner stdout log and
rewrites the matching `int Name = N;` lines in `src/eval_params.h`.
Original is backed up to `eval_params.h.bak`.

Idempotent — re-running produces the same result.

PowerShell users: pipe through Set-Content -Encoding utf8 first
(PowerShell writes UTF-16 by default which the script can't parse).

### Bench convention

`Hypersion bench [depth]` runs 8 fixed positions at the specified
depth (default 13). With `setoption name Threads value 1`, the output
is deterministic. The project-canonical depth-13 count is stored in
`BENCH_SIGNATURE`; both the smoke test and `make verify` read it rather
than duplicating a stale number in multiple places. Search or evaluation
changes that intentionally alter the tree must update the signature in
the same change.

### Position datasets (for tuning)

- `tuner_positions.txt` (12 MB, 221k positions) — Hyp self-play
- `tuner_positions_3M.txt` (180 MB, 3M positions) — master games
- `tuner_positions_large.txt` (993 MB, 16M positions) — master games

See `tools/tuner/README.md` for tune methodology.
