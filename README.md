# Hypersion

A free, open-source UCI chess engine.

Hypersion uses an NNUE network for evaluation (Stockfish 18 SFNNv10
architecture: HalfKAv2_hm features + FullThreats, big network 1024-d FT,
small network 128-d FT) on top of an alpha-beta search with PVS,
transposition table, aspiration windows, late-move reductions, singular
extensions, ProbCut, futility/razoring/SEE pruning, and lazy-SMP
infrastructure (single-threaded by default — multi-thread is currently
unstable).

## Features

- **Evaluation**: SF18-style NNUE inference with incremental accumulator
  updates, Finny refresh cache, and runtime small/big network selection.
- **Search**: PVS + aspiration windows + LMR + singular extensions +
  multi-cut + ProbCut + razoring + futility + SEE move pruning +
  correction history + counter-moves + 2-ply continuation history.
- **Adaptive strength**: `UCI_LimitStrength` + `UCI_Elo` map a target
  rating from ~500 to ~3200 to a depth cap and move-selection noise.
- **Build**: single Makefile, AVX2 default, optional AVX-VNNI / AVX-512
  / native-arch targets, optional 2-pass PGO.
- **Tablebases**: Syzygy probing via the bundled Fathom library.
- **Opening book**: optional Polyglot `.bin` book.

## Strength

Vs. UCI_Elo-limited Stockfish on `nn-c288c895ea92.nnue`, 16-game matches
at 5+0.05 (bullet) with concurrency=2:

| SF UCI_Elo | Hypersion result | Score % |
|---|---|---|
| 2000 | +16 =0 -0  | 100 % |
| 2200 | +15 =1 -0  | 96.9 % |
| 2400 | +9 =5 -2   | 71.9 % |
| 2600 | +11 =0 -5  | 68.8 % |
| 2800 | +4 =1 -11  | 28.1 % |

Bullet TC strength estimate: **≈ 2500-2600 SF-Elo**. Slower time controls
shift higher.

Tactical suite results @ fixed depth (199 / 1277 / 901 positions):

| Suite | Depth | Solved |
|---|---|---|
| WAC (Win at Chess) | 12 | 96.0 % |
| mate-in-3 | 8 | 96.1 % |
| mate-in-5 | 12 | 87.6 % |

## Building

Requires `g++` 12+ with C++20 support and GNU make. From the project
root:

```
make build              # default release build (AVX2 + LTO)
make bench              # build then run a deterministic NPS bench
```

Architecture targets:

```
make build ARCH=x86-64-avx2     # default; works on most modern CPUs
make build ARCH=x86-64-bmi2     # adds PEXT (Zen 3+, Ice Lake+)
make build ARCH=x86-64-avxvnni  # +AVX-VNNI dpbusd (Alder Lake+)
make build ARCH=x86-64-avx512   # AVX-512 (Skylake-X+, Zen 4+)
make build ARCH=native          # auto-detect via -march=native
```

For modern Intel (12th gen+) / AMD (Zen 4+) hardware, the AVX-VNNI build
is **+29.6 ± 35.6 ELO** over the AVX2 default at 200g 5+0.05 conc=2 —
recommended for distribution to such users.

Distribution-ready stripped binaries:

```
py testing/build_releases.py   # builds avx2/bmi2/avxvnni stripped variants
                               # into ./release/, 1.32 MB each
```

See [docs/BUILDING.md](docs/BUILDING.md) for full options including PGO,
debug builds, and the bundled Texel tuner.

## NNUE network files

The shipping evaluation needs two NNUE files in the engine's working
directory:

- `nn-c288c895ea92.nnue` — big network (~104 MB)
- `nn-37f18f62d772.nnue` — small network (~3 MB)

These are **not** included in the repository. Download instructions and
direct links: [docs/NNUE.md](docs/NNUE.md).

If neither file is present, Hypersion falls back to its classical
evaluator. It still plays — just much weaker.

## Running

After building and dropping the two `.nnue` files next to the executable:

```
./Hypersion
> uci
> position startpos
> go movetime 1000
> bestmove e2e4 ponder ...
```

It speaks UCI 1.0; any UCI-compatible GUI (Cute Chess, Arena, BanksiaGUI,
ChessBase, Scid, Fritz, …) will drive it.

## UCI options

| Option | Default | Range | Notes |
|---|---|---|---|
| `Hash` | 16 | spin | TT size in MB |
| `Threads` | 2 | spin | lazy-SMP works; 2 is a safe default. For deterministic bench/testing, use 1. |
| `EvalFile` | `nn-c288c895ea92.nnue` | string | big NNUE network |
| `EvalFileSmall` | `nn-37f18f62d772.nnue` | string | small NNUE network |
| `SyzygyPath` | empty | string | Syzygy tablebase directory |
| `BookFile` | `Perfect2023.bin` | string | optional Polyglot book |
| `OwnBook` | false | check | use the opening book |
| `BookBestMove` | false | check | always pick highest-weight book move |
| `Skill Level` | 20 | 0–20 | weaker play at low values |
| `UCI_LimitStrength` | false | check | enable Elo limiting |
| `UCI_Elo` | 1500 | 500–3200 | target rating when limit-strength is on |
| `UCI_AnalyseMode` | false | check | hint that the GUI is analysing, not playing |
| `Move Overhead` | 30 | spin | ms reserved for GUI / network latency |
| `Contempt` | 0 | −200 to 200 | draw aversion |

## Repository layout

```
Hypersion/
├── src/                  C++ engine source
│   └── fathom/           bundled Syzygy probe (Fathom)
├── tools/tuner/          Texel-style tuner for classical eval
├── testing/              cutechess-cli launchers (edit paths to your env)
├── docs/                 build + NNUE-download guides
├── .github/workflows/    CI build job
├── Makefile
├── LICENSE               GPL-3.0-or-later
├── AUTHORS
└── README.md
```

## Acknowledgements

- **Anthropic's [Claude](https://claude.com)** — Hypersion was developed
  with substantial implementation help from Claude, including the NNUE
  port from Stockfish 18, incremental accumulator updates, the Finny
  refresh cache, search-margin retuning for NNUE, and many other parts
  of the engine. Architectural direction was RenCopp's; Claude wrote and
  audited the code under that direction.
- **Stockfish** — Hypersion uses Stockfish 18's NNUE network files
  (`nn-c288c895ea92.nnue`, `nn-37f18f62d772.nnue`) and ports its NNUE
  inference algorithm (HalfKAv2_hm + FullThreats). Both are GPL-3
  licensed and Hypersion inherits that license.
- **Fathom** — Syzygy tablebase probing (bundled in `src/fathom/`).
- The chess-programming community at <https://www.chessprogramming.org>.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
