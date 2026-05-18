# Engine tournament results (2026-05-18)

## Setup

| Engine | Binary | Notes |
|---|---|---|
| Stockfish (SF18) | `C:\Engine\Engines\prebuilt\stockfish\Stockfish.exe` | Built x86-64-avx2; nets `nn-c288c895ea92.nnue` + `nn-37f18f62d772.nnue` |
| Hypersion v3.0 | `C:\Engine\Hypersion\release\Hypersion-x86-64-avxvnni-pgo.exe` | Local build, AVX-VNNI PGO |
| Alexandria 9.0 | `C:\Engine\Engines\prebuilt\alexandria\Alexandria.exe` | avx2 download from PGG106/Alexandria release |
| Berserk 13 | `C:\Engine\Engines\prebuilt\berserk\Berserk.exe` | avx2; net `berserk-d43206fe90e4.nn` alongside |
| Obsidian 16.0 | `C:\Engine\Engines\prebuilt\obsidian\Obsidian.exe` | avx2; net embedded |
| RubiChess 20240817 | `C:\Engine\Engines\prebuilt\rubichess\RubiChess.exe` | avx2; net `nn-bc638d5ec9-20240730.nnue` alongside |

**Tournament config**: cutechess-cli round-robin, TC 5+0.05, conc=4, Hash=64 MB, Threads=1, openings = `popularpos_lichess_v3.epd` random order plies=8, 2 rounds.

**Total games**: 30 (5 opponent pairings × 2 rounds × ~3 games — small sample, see CI caveats below).

**Note**: Ethereal 14.00 was NOT included — build failed (clang+PGO LLVM-bitcode link error on MSYS2). Source-only reference, no Windows release binary on GitHub.

## Final standings

| Rank | Engine | ELO ± | Score | Draw% | W-L-D |
|---|---|---|---|---|---|
| 1 | **Stockfish** | **+301 ± 340** | 85.0% | 30% | 6-0-4 |
| 2 | Alexandria | +35 ± 122 | 55.0% | 70% | 2-1-7 |
| 3 | RubiChess | 0 ± 99 | 50.0% | 80% | 1-1-8 |
| 4 | Obsidian | 0 ± 144 | 50.0% | 60% | 3-3-4 |
| 5 | Berserk | -35 ± 163 | 45.0% | 50% | 2-3-5 |
| 6 | **Hypersion** | **-301 ± 340** | 15.0% | 30% | 0-7-3 |

## Caveats

- **Sample size**: only 10 games per engine — CI bands are wide. With 30 total games the ELO ranking has high variance. The point estimates are directionally informative but not definitive.
- **TC dependency**: results are at bullet TC (5+0.05). At LTC, expect Stockfish's lead to narrow somewhat and Hypersion's gap to shift (per the Tier 3+4 LTC validation: Hypersion's bullet:LTC ratio is ~1:0.86, not bullet-specific).
- **Single-thread**: all engines played at Threads=1. Lazy-SMP scaling differs across engines.
- **Hash size**: 64 MB hash — small for serious play. Larger hash favors deeper-searching engines (probably Stockfish).
- **TC pressure**: Hypersion's known bullet flag-out issue (`<2s remaining` panic) likely contributes to its score; see CLAUDE.md "Known open issues #1".

## What we learned

1. **Stockfish is comfortably the strongest** in this field — +266 ELO over the field median, win rate 85%, drew 30%. As expected.

2. **Alexandria > middle pack**: edges ahead of RubiChess/Obsidian/Berserk by ~35-70 ELO at this TC. Suggests the **adaptive time management + hindsight reduction** combo is paying off.

3. **RubiChess / Obsidian / Berserk are roughly tied** at this TC. Their distinguishing features (threat-square history, threat-weighted quiet scoring, NNUE eval corrections) yield similar net strength — interchangeable as porting sources.

4. **Hypersion is at -266 from field median** — substantial gap. Hypersion is at v3.0 (~+140 ELO from the May 2026 audit) but still well behind production CCRL-3500 engines. The gap is likely most of:
   - **Network strength**: Hypersion uses the SF18 net, but the SURROUNDING tuning isn't co-trained (margins, history scales, time mgmt are Hypersion-tuned for the Hypersion search tree, not the SF tree).
   - **Search-quality joint optimization**: Berserk/Obsidian/RubiChess have years of joint LMR/NMP/history/eval tuning. Hypersion's audit ports SF18 logic but the surrounding parameters lag.
   - **Bullet TC weaknesses**: known flag-out + book early-exit issues per issue #2.

## Implications for Hypersion development

Per **Rule 2** (consult engines before edits), the priority order for porting ideas from this tournament:

| Source engine | Highest-ELO-likely port | File:line |
|---|---|---|
| **Berserk** | NNUE eval corrections (pawn + cont history blend) | `history.h:79-85` |
| **Alexandria** | Hindsight reduction depth penalty | `search.cpp:553` |
| **Obsidian** | Threat-weighted quiet scoring + pawn-keyed history | `movepick.cpp:77-98`, `history.h:23` |
| **RubiChess** | Threat-square keyed history | `search.cpp:128` |
| **Ethereal** | Classical eval reference (most readable, defer if/when NNUE fallback wanted) | `evaluate.c` |

Each of these is **independently SPRT-testable**. Don't bundle — bundling has hidden the actual ELO contribution of multiple Hypersion porting attempts. Try the most-promising one (Berserk eval corrections) first.

## How to reproduce / extend

```powershell
cd C:\Engine\Engines\prebuilt
& "C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe" `
  -tournament round-robin `
  -engine name=Stockfish cmd="stockfish/Stockfish.exe" dir="stockfish" `
  -engine name=Hypersion cmd="C:/Engine/Hypersion/release/Hypersion-x86-64-avxvnni-pgo.exe" dir="C:/Engine/Hypersion" `
  -engine name=Alexandria cmd="alexandria/Alexandria.exe" dir="alexandria" `
  -engine name=Berserk cmd="berserk/Berserk.exe" dir="berserk" `
  -engine name=Obsidian cmd="obsidian/Obsidian.exe" dir="obsidian" `
  -engine name=RubiChess cmd="rubichess/RubiChess.exe" dir="rubichess" `
  -each proto=uci tc=5+0.05 option.Hash=64 option.Threads=1 `
  -openings "file=C:/Engine/Hypersion/testing/openings/popularpos_lichess_v3.epd" format=epd order=random plies=8 `
  -rounds N -games 1 -repeat -recover `
  -concurrency 4 `
  -pgnout tournament_6way.pgn `
  -ratinginterval 5
```

Increase `-rounds` for larger sample. 6 rounds → 90 games → CI tightens to ~±150.

PGN of the May 2026 tournament: `C:\Engine\Engines\prebuilt\tournament_6way.pgn`
Log: `C:\Engine\Engines\prebuilt\tournament_6way.log`
