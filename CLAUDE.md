# Hypersion — project context for Claude Code

You are working on **Hypersion**, an AVX2 NNUE chess engine in C++20 derived from
Stockfish-style architecture but tuned independently. Author: RenCopp.

## Quick orientation

- **Source layout**: `src/` (engine), `testing/` (SPRT + analysis scripts),
  `Hypersion.exe` at repo root after `make`.
- **Build**: `make -j` from MSYS2 MinGW64 (default `ARCH=x86-64-avx2`,
  `-O3 -flto`). For Alder Lake+ / Zen 4+ CPUs, use
  `make ARCH=x86-64-avxvnni -j` for **+29.6 ± 35.6 ELO** (200g, 5+0.05,
  conc=2) — uses 256-bit VNNI `dpbusd` for the NNUE FC dot product.
- **Bench**: `Hypersion bench [depth]` — 8 fixed positions, default depth 13.
  *Note: bench inherits `Threads` setting (default 2 = lazy SMP non-deterministic).
  For deterministic bench, prefix with `setoption name Threads value 1`. Earlier
  "non-deterministic at Threads=1" claim was an artefact of not setting Threads
  explicitly; resolved 2026-05-07. See testing/PROTOCOL.md.*
- **NNUE**: SF18 SFNNv10 architecture (HalfKAv2_hm + FullThreats), big net
  `nn-c288c895ea92.nnue` (102384, 1024, 15, 32, 1), small net
  `nn-37f18f62d772.nnue` (22528, 128, 15, 32, 1). L2=15, L3=32.

## SPRT testing protocol — read first

`testing/PROTOCOL.md` is the source of truth. Summary:
- **Stage 1** (30g triage, conc=6, TC 5+0.05): outcome ≤ -50 ELO → REJECT,
  [-50, +50] → NOISE, > +50 → run Stage 2.
- **Stage 2** (200g, same TC): ≤ +5 with CI ±35 → REJECT (tombstone), > +10 → SHIP.
- **Memory-aggressive optimizations** (NNUE SIMD, weight layout, PGO, eval
  cache) are **masked at concurrency=6** — use **concurrency=2** for those.
  See cutechess #630 / OpenBench rationale in PROTOCOL.md.

**Tombstone every rejected result.** Source-inline comment with ELO ± CI,
sample size, reason, future-contributor hint. Examples in `src/search.cpp`,
`src/history.h`, `src/nnue.cpp`.

## Key local references

- **Stockfish source**: `C:\Engine\stockfish\src\` — full reference impl,
  read-only. Use Read/Grep here, not WebFetch — much faster.
- **Stockfish wiki**: `C:\Engine\stockfish\wiki\` — UCI commands,
  terminology, regression tests.
- **Stockfish binary**: `C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe`
  — used as ground truth for blunder analysis (`testing/vs_stockfish.py`).
- **cutechess**: `C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe`
- **Syzygy 3-4-5**: `C:\Engine\3-4-5 syzygy` — endgame TBs.

## Useful scripts

- `testing/sprt.py` — SPRT runner (BASE vs candidate)
- `testing/vs_stockfish.py` — match Hypersion vs Stockfish, surface blunders
- `testing/analyze_with_sf.py` — per-move SF eval comparison from PGN
- `testing/wac_runner.py` — WAC tactical suite

## Known open issues

1. **Bullet flag-out** with passed pawn + winning advantage — engine "panics"
   under <1s remaining. Mitigation: load Syzygy. See timeman tombstone in
   `search.cpp` for failed fix attempts. Investigation queued (testing/
   test_bullet_conversion.py exists).
2. **Search at tight local optimum** — single-feature SF18 ports keep
   regressing because the engine's pruning margins, history bonuses, and
   eval magnitudes are jointly tuned for the existing feature set. To
   move forward: joint multi-feature SPSA over coordinated parameter
   groups, NOT one-at-a-time ports. Tombstones in src/search.cpp
   (tunables namespace), src/history.h, src/nnue.cpp document the
   pattern.

## Resolved (kept for context)

- **Bench non-determinism** at Threads=1 — was lazy SMP at default
  Threads=2. Bench IS deterministic with `setoption name Threads value 1`
  set explicitly. Resolved 2026-05-07. See PROTOCOL.md "Bench Signature".

## Recent ship history (newest first)

Run `git log --oneline -20` to see recent commits. Look for commits with
"tombstone" or "ship" in message — they document what was tested.

Most recent SHIPPED state: commit ebf491f (2026-05-08), Hypersion v2.1
+118 ELO over pre-overhaul baseline. The session leading up to it tested
8+ SF18-style ports (LowPlyHistory, 6-deep contHist, mate-threat extension,
SE double-extension, eval cache, NMP verification, history-update cluster,
12-param SPSA at 4g/iter) — all rejected. The infrastructure for runtime
SPSA tuning (UCI Tune_* options, set_tunable, testing/spsa.py) stays
shipped for future joint-parameter campaigns.

## Specialized skill

For deep chess-programming knowledge (SF18 NNUE arch, search heuristics,
common bugs), invoke `/chess-engine-dev`. The skill is at
`.claude/skills/chess-engine-dev/SKILL.md`.

## Slash commands

- `/commit` — analyses staged + unstaged changes, drafts a commit message
  matching the repo's style (descriptive subject + tombstone-aware body),
  stages relevant files, creates the commit.
- `/commit-push-pr` — same as `/commit` plus push + `gh pr create`.
- `/clean_gone` — remove local branches whose remote tracking is gone.

## C++ language server (clangd)

clangd is set up for semantic navigation, find-definition, find-references,
clang-tidy diagnostics. Files:
- `compile_commands.json` (gitignored) — regenerated by
  `py testing/gen_compile_commands.py` after adding/removing src files.
- `.clangd` — points to MSYS2 g++ (15.2.0) so std headers + intrinsics resolve.

Use the LSP-aware tools (Read/Edit) and trust their output. If clangd starts
flagging false positives, regenerate compile_commands.json — most "errors"
come from stale flags.

## Style guidelines

- **Tombstone don't delete**: every failed experiment leaves a comment so
  the next contributor doesn't re-test it.
- **Cite ELO ± CI** in tombstones. "Didn't help" is not a tombstone; "-23
  ± 38 ELO @ 200g 5+0.05" is.
- **Reference SF source by path** when porting: `// Source: SF18
  src/movepick.cpp:score()`. Future readers can look it up.
- **No new files unless necessary**. Existing structure is intentional.
- **No emojis** in source or commit messages.
