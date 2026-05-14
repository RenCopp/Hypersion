# Hypersion

A free, open-source UCI chess engine.

Hypersion uses an NNUE network for evaluation (Stockfish 18 SFNNv10
architecture: HalfKAv2_hm features + FullThreats, big network 1024-d FT,
small network 128-d FT) on top of an alpha-beta search with PVS,
transposition table, aspiration windows, late-move reductions, singular
extensions, ProbCut, futility/razoring/SEE pruning, and lazy-SMP
infrastructure.

> **Project status (v3.0+, 2026-05-14):** active search/move-ordering
> development is **paused**. The engine is at a local optimum on its
> current SF18 NNUE network — 9 SPSA campaigns and dozens of tombstoned
> experiments have explored the remaining parameter regions. The most
> realistic remaining ELO ceiling is an NNUE retrain. Development
> resumes if a contributor steps up with a custom-trained NNUE network.
> See [CONTRIBUTING](#contributing) below.
>
> **Classical-eval expansion session (2026-05-14)**: classical eval
> grew from 14 to 145 tunable scalars across 16 feature rounds
> (Mop-up, OCB scaling, Initiative bonus, KBNK mating, drawish-endgame
> scaling, KPK bitbase, plus 9 other feature blocks). Best WAC depth-8
> tactical: **184/198 (92.9%)**. NNUE-shipped path bench is **unchanged
> at 1,273,328 nodes** (T1 d=13) — classical-only changes don't reach
> the shipping eval path. See `testing/IMPROVEMENTS_LOG.md`.

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

v3.0 cumulative gains over v2 (sum of 6 SPRT-confirmed shipped
improvements at TC 5+0.05, conc=6, each measured against the immediate
prior baseline):

| Improvement | ELO @ sample size |
|---|---|
| Bullet flag-out fix | +22.6 ± 37 (200g) |
| A2-v2 history weights | +27.9 ± 17 (400g) |
| A3 search margins | +33.1 95% CI (+11.9, +54.6) (600g) |
| A5 LMR-statScore divisor | +38.4 95% CI (+16.7, +61.1) (600g) |
| A9 joint-cluster retune | +28.8 95% CI (+3.5, +54.7) (400g) |
| A10 falling-eval-divisor | +27.2 95% CI (+1.4, +53.0) (400g) |

**Bullet TC strength estimate**: ≈ 2600-2700 SF-Elo (up from v2's
~2500-2600). Long-TC validation (60+0.6, 200g): +17.4 ELO with 95% CI
(-21, +56) — meaningfully positive but not statistically conclusive at
the LTC sample size used. Most session gains are bullet-specific.

Tactical suite results @ fixed depth (300 / 130 / 130 positions):

| Suite | Depth | Solved (NNUE) | Solved (classical-only) |
|---|---|---|---|
| WAC (Win at Chess) | 12 | 94-96 % | 184/198 = 92.9 % |
| mate-in-3 | 8 | ~96 % | ~97 % |
| mate-in-5 | 12 | ~88 % | n/a |

Classical-only WAC is measured at depth 8 (faster validation for
Texel tuning regression checks). The 184/198 result is the project
best across the entire 16-round eval expansion (2026-05-12/14).

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
| `Hash` | 64 | spin | TT size in MB |
| `Threads` | 2 | spin | lazy-SMP works; 2 is a safe default. For deterministic bench/testing, use 1. |
| `EvalFile` | `nn-c288c895ea92.nnue` | string | big NNUE network |
| `EvalFileSmall` | `nn-37f18f62d772.nnue` | string | small NNUE network |
| `SyzygyPath` | empty | string | Syzygy tablebase directory |
| `BookFile` | `Perfect2023.bin` | string | optional Polyglot book |
| `OwnBook` | true | check | use the opening book |
| `BookBestMove` | false | check | always pick highest-weight book move |
| `Skill Level` | 20 | 0–20 | weaker play at low values |
| `UCI_LimitStrength` | false | check | enable Elo limiting |
| `UCI_Elo` | 1500 | 500–3200 | target rating when limit-strength is on |
| `UCI_AnalyseMode` | false | check | hint that the GUI is analysing, not playing |
| `UCI_Opponent` | empty | string | opponent info from GUI (`<title> <rating> <type> <name>`) |
| `UCI_MatchOpponent` | false | check | auto-match opponent ELO (offset curve, see below) |
| `UCI_GameRated` | false | check | per-game rated/casual flag (set by lichess-bot) |
| `UCI_GameTournament` | false | check | per-game tournament flag — when true, ELO matching applies even in rated games |
| `Move Overhead` | 30 | spin | ms reserved for GUI / network latency |
| `Contempt` | 0 | −200 to 200 | draw aversion |

The opponent-matching behavior with `UCI_MatchOpponent=true`:

| Game type | Engine plays |
|---|---|
| Rated, non-tournament | Full strength (preserves global ELO) |
| Rated tournament | Match opponent ELO (-50 underbid, see curve below) |
| Casual, any opp w/ rating | Match opponent ELO |
| Anything w/o opp rating | Full strength |

Offset curve (bot plays N ELO below opponent rating):

| Opp rating | Bot underbid |
|---|---|
| <800 | -175 |
| 800-1200 | -150 to -175 |
| 1200-1600 | -150 |
| 1600-2000 | -125 |
| 2000-2400 | -100 |
| 2400-2600 | -75 |
| 2600+ | -50 |

## Contributing

Hypersion's active development is paused after v3.0. The single biggest
remaining ELO lever is an **NNUE network retrain**. The architecture
(SF18 SFNNv10) and training pipeline (`nnue-pytorch`) are public; the
blocker is GPU compute.

If you're interested in contributing:

- **NNUE retrain** (high impact, +30 to +60 ELO realistic): use
  `nnue-pytorch`, train a fresh net on Hypersion self-play data or the
  public Stockfish training datasets (e.g. `linrock/test80-2024` on
  HuggingFace). Drop the resulting `.nnue` file into the engine's
  working directory; existing NNUE inference loads it with no source
  changes if architecture matches.
- **Long-TC SPSA** (moderate impact): the session SPSA campaigns ran at
  `nodes=50000` (bullet-equivalent depth ~10-12). LTC-specific tuning
  campaigns (`nodes=500000+` or `tc=60+0.6`) should find further gains.
- **Bug reports / specific-position blunders**: open issues with the
  problematic FEN and Hypersion's response.

Open an issue or PR at <https://github.com/RenCopp/Hypersion>.

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
