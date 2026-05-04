# Hypersion CHANGELOG

## Release policy

Don't tag a new GitHub release until **at least 8 verified improvements**
have stacked on `main` AND a head-to-head A/B match against the previous
release shows **non-regressing strength** (≥ −10 ELO, ideally ≥ +20).
Bigger ELO swings (+50, +200) are bonus targets, not gates.

This keeps releases meaningful for casual users who download the zip,
without holding shipping hostage to multi-month ELO grinds. Daily dev
work stays on `main` (no release needed).

---

## v2 (released 2026-05-04)

Tagged `v2`. Source: commit at the v2 tag.

### UCI_LimitStrength — complete rewrite + Maia/dala calibration

The v1 strength limiter was found to be **broken** (testing/test_elo_scaling.py
at internal monotonicity exposed it: Hypersion@900 beat Hypersion@1100 by
**95 %** because every level under UCI_Elo 2000 played near-randomly —
best-move probability ~5 % regardless of skill setting).

Replaced with two clean levers:
- **Per-bucket node cap** (lookup table). Caps search work, smooth across
  all UCI_Elo values. No depth caps — those caused horizon-effect
  inversions where lower-depth-but-still-tactical search outperformed
  slightly-deeper search.
- **Low-rate blunder probability** — small chance per move to pick a
  sub-optimal move from the top half of root moves. Reduced from 45 %
  to 8 % at ELO 500 specifically because user feedback called the high
  rate "obvious random play". Weakness now comes mostly from limited
  search depth, which produces natural-looking weak moves.

Validated against real human-trained bots:
- **Maia 1100/1500/1900** ([CSSLab](https://github.com/CSSLab/maia-chess))
- **dala-700/900** ([hrschubert](https://github.com/hrschubert/dala-training))

| Hypersion@N | vs | Score | Verdict |
|---|---|---|---|
| @700 | dala-700 (~881 actual) | 25.0 % | Bullseye — matches expected 26 % for true 700 vs 881 |
| @900 | dala-900 (~1000 actual) | 60.0 % | OK (slightly strong, within 10-game noise) |
| @1100 | Maia 1100 | 45.0 % | OK |
| @1500 | Maia 1500 | 50.0 % | OK |
| @1900 | Maia 1900 | 40.0 % | OK |

### Search refinements

- LMR formula softening (1.95 → 1.90 in the log/log table).
- NMP zugzwang strengthening at depth ≥ 12 (require non_pawn_material
  > knight value).
- Endgame LMR mitigation: subtract 1 ply when total piece count ≤ 8.
- Singular extension threshold 6 → 5 (matches Stockfish 18).
- Stockfish-18 `fallingEval` time scaling — score-trend-based per-iteration
  scale factor in [0.6, 1.7].
- NNUE big-net threshold 962 → 1500 — small NNUE was activating for
  K + R-class endgames where conversion accuracy matters.

### Build

- AVX-VNNI build target with `-march=alderlake -mavxvnni`.
  Bench NPS up ~9 % (median over 7 samples) on Intel 12th gen+ /
  AMD Zen 4+ CPUs. v1 was AVX2-only with no `vpdpbusd` instructions.
- 64-byte cache-line alignment on NNUE accumulators and on-stack
  buffers in the inference path.
- PGO Makefile fix: `make profile` now succeeds with
  `-Wno-coverage-mismatch -Wno-error=coverage-mismatch` on the
  use pass.

### Bug fixes

- **Repetition during search**: `Worker::prepare()` now deep-copies the
  full StateInfo chain. v1 discarded history, so search couldn't see
  a 3-fold repetition still pending from the actual game (engine
  evaluated perpetuals as winning). User-reported case from
  https://lichess.org/0ljEy9Fu.
- **50-move-rule + checkmate** edge case in position.cpp.
- **Cosmetic UCI eval scale**: `cp` output now uses Stockfish's
  "1 pawn = 100 cp" convention.

### Tooling

- `testing/sprt.py` — cutechess-cli wrapper with live SPRT tracking.
- `testing/test_elo_scaling.py` — internal Hyp@N vs Hyp@M monotonicity.
- `testing/test_vs_maia.py` — Hypersion vs Maia at matched ELO.
- `testing/test_vs_dala.py` — Hypersion vs dala bots.
- `testing/test_full_elo_grid.py` — full 500–2500 grid.
- `testing/test_bullet_conversion.py` — low-time winning-position smoke.
- Default opening book switched from `eco.bin` (polyglot, mis-passed
  as EPD) to `popularpos_lichess_v3.epd` (200 k real positions, CC0-1.0).

### Investigated but reverted

Tombstone-commented in source so future work doesn't retry blind:

- SF18 continuation-history pruning at low depth (regressed −207 ELO).
- NMP base R bump 4 → 5 (regressed −50 ELO).
- Material-keyed correction history (regressed −26 ELO).
- LMR `opponentWorsening` adjustment (−33 ELO).
- SEE-quiet `opponentWorsening` (−47 ELO).

`opponentWorsening` flag itself was kept where structurally sound
(RFP / futility / razoring / probcut margins) — within ±35 ELO noise
in direct measurement, theoretically positive in the conversion-aware
direction.

---

## Post-v2 patches (on `main`, not yet tagged)

### Lichess-bot config: `move_overhead` 2000 ms → 200 ms

The lichess-bot wrapper was reserving **2 seconds per move** as
"network safety margin" (the framework's default). On a 60+0 bullet
game, this leaves 58 s for ~40 moves vs. an opponent that has the
full 60 s — Hypersion was effectively flagging or playing very
shallow moves through no fault of its own search.

Fixed in `lichess-bot-master/config-hypersion.yml`:
* `move_overhead: 2000` → `200` (top-level / wrapper)
* `Move Overhead: 300` → `100` (engine UCI option)

Total per-move safety reserve: **300 ms** (was 2300 ms).  This
should give a noticeable strength bump in bullet/blitz games on
lichess.

### Close-major-pieces crash bug — RESOLVED

The `ACCESS_VIOLATION 0xC0000005` crash on positions like
`8/4kp2/3p4/3P1q2/8/4Q3/5PK1/8 w - - 0 1` through python-chess
piped stdio (documented in this CHANGELOG as "NOT YET FIXED") is
no longer reproducible.

Verified with:
* `testing/test_crash_repro.py` — 7 close-major-piece positions, all OK.
* `testing/test_crash_stress.py` — 200 analyses across 8 positions
  through python-chess `SimpleEngine.popen_uci`, **0 crashes** in 34 s.

Most likely fixed by the v2 alignas(64) + AVX-VNNI rebuild — the
bug had the signature of memory mis-alignment colliding with SIMD
loads under specific stack-pressure scenarios.

---

## Unreleased (in progress on `main`)

Currently stacked, not yet tagged:

1. Insufficient-material instant draw (KvK, KvKB, KvKN, KBvKB-same-colour)
2. `go searchmoves` UCI command
3. Bishop same-colour bug fix (was buggy across ranks)
4. PSQT bucket SIMD (`_mm256_add_epi32` / `_mm256_sub_epi32` for the 8
   PSQT bucket entries) — +7 % bench NPS, NNUE-validated bitwise neutral
5. `UCI_ShowWDL` option — outputs `wdl W D L` (per-mille) in info lines
6. Hash default 16 → 64 MB
7. Bench output OpenBench-compatible (per-position lines + `Nodes searched : N` signature)
8. Compile-arch info-string at startup
9. `UCI_Variant` declaration **removed** — was rejecting python-chess's
   `"chess"` value and aborting every lichess-bot game with EngineError
10. **Book variety** — lower opening-book filter threshold (`bestW/4` →
    `bestW/12`) and switch to uniform weighting (was sqrt-weighted) among
    surviving moves. Same opening family, but real shuffle within playable
    lines — playing the same opponent twice gives different games. Pure
    user-facing behaviour change, no claimed strength impact.
11. **Stockfish-18 LMR history correction (quiet moves)** — in the Late
    Move Reductions block, after the static adjustments (improving / cutNode
    / PV / etc.), reduce `r` further by `statScore / 8192`, where
    `statScore = 2·butterfly + contHist[0] + contHist[1]`.
    High-history quiets get reduced less; low-history get reduced more.
    Stockfish uses `/11248` against a 4-ply contHist sum that Hypersion
    doesn't have; `/8192` keeps the effective range similar with 2-ply.
    A/B vs v1.0: **+72.2 ELO** (77W/87D/36L over **200 games** at TC 10+0.1,
    95% CI [+24.1, +123.3] — entirely above zero, first confirmed improvement
    on `main` since v1.0).

Cumulative effect on `main` vs Hypersion 1.0:
**+72 ELO** (LMR history correction, validated at 200 games).
Bench (depth 11): 648,118 nodes (deterministic) vs 596,919 for v1.0.

Tested-but-reverted (added in this session):
- Capture-side LMR history correction — A/B vs lmrhist (200g) was
  -22.6 ELO with CI [-72, +26], statistically flat / point-estimate
  negative. Reverted in `140d66b`.

Tested-but-no-effect (kept off):
- Syzygy 3-4-5 tablebases — isolated A/B (200g, same binary, only
  difference is `SyzygyPath` set vs not) gave -13.9 ELO with CI
  [-63, +34], W/D/L = 20/152/28 = statistically null. Game-mining showed
  only 3 / 31 losses (10%) reached the 5-piece zone where Syzygy could
  help. Tablebase code works correctly (probe verified) but doesn't
  swing matches at this time control.
- 4-ply continuation history (commit `395f889`, reverted in `c9a1909`).
  Added contHist[2] tracking (ss-4)'s move and read it in LMR statScore
  correction (SF-18 pattern). Cutoff-handler updates with `bonus/4`
  weight (1-ply: full, 2-ply: half, 4-ply: quarter). 18-game partial
  result before match stalled: W=5 D=9 L=4 = 52.8% = +19 ELO trend with
  CI ±170 (well within noise of zero). Match was unusually slow (~30 min
  for 18 games vs typical ~4-5 min for 30g). Reverted to keep baseline
  clean — the small possible positive isn't worth the memory overhead
  and timing instability.

---

## Features added

- **`UCI_Opponent` + `UCI_MatchOpponent`** (commits `601a951`, `11d318d`,
  `1ebcd50`): opponent-aware strength matching for lichess deployment.
  python-chess sends opponent info as `<title> <rating> <player_type>
  <name>` (e.g. `none 600 human Joe` or `none 2400 computer Bot`).
  When `UCI_MatchOpponent=true`:
  - Bots / engines / computers: full strength, no limit
  - Humans: graduated offset curve (educational ZPD)
        <1000      +150
        1000-1399  +100
        1400-1799  +75
        1800-2199  +50
        2200-2599  +25
        >=2600     0      (master+ exact match)
  - Final UCI_Elo clamped to [600, 3200]
  - Each new game resets to full strength first, so a previous human-game
    limit doesn't carry over to the next bot game
  - Default off; bench 648,118 unchanged in cutechess testing

- **`UCI_GameRated`** (commit `3f4f920`): per-game rated/casual flag set
  by lichess-bot's `extra_game_handlers.py`. When true, opponent matching
  is suppressed and the engine plays at full strength regardless of
  opponent — rated games count for ELO and shouldn't be deliberately
  weakened. Decision matrix:
        rated game (any opp)   -> full strength
        casual + bot opp       -> full strength
        casual + human opp     -> matched UCI_Elo

- **Bug-fix sweep + SMP diversity** (commit `a6e9e17`):
  - `position.cpp` 50-move-rule now correctly excludes checkmate
    (`if (rule50 > 99 && (!checkers() || MoveList<LEGAL>(*this).size() > 0))`)
  - `position.cpp` repetition walk now null-checks the StateInfo chain
  - SMP works at Threads >= 2 (the "buggy" warning was stale). Default
    bumped from 1 -> 2. Helpers diversify via Stockfish-style depth
    skipping (SKIP_SIZE/SKIP_PHASE arrays from SF18). Bench at Threads=1
    still 648,118 nodes (canonical baseline preserved). Bench at
    Threads=2: 596,944 nodes / 564k NPS / 1.06s wall (vs 360k / 1.8s
    single-threaded) = ~1.7x speedup.
  - 4 standard UCI options added for Stockfish/GUI compatibility:
    SyzygyProbeDepth, Syzygy50MoveRule, SyzygyProbeLimit, UCI_Chess960

- **ttPv tracking + I/O unbuffering** (commit `460c575`):
  - TT entry now persistently stores a "principal-variation" bit (packed
    into bit 7 of depth8; depth field truncated to 7 bits = max 127).
    On probe, ttPv = isPv || (ttHit && tte->is_pv()) — sticky once set.
  - LMR reduction relaxed by 1 ply on ttPv-tracked positions
    (`if (ttPv) --r`). Avoids over-pruning lines that have ever been part
    of a principal variation.
  - Bench at Threads=1: 648,118 -> 823,255 nodes (+27%). The reduction
    relaxation makes search explore more at fixed depth.
  - main.cpp: full I/O unbuffering for piped stdio (Win32 anonymous pipes
    used by lichess-bot, cutechess, python-chess subprocess.PIPE).
    sync_with_stdio(false) + cin.tie(nullptr) + setvbuf(IONBF) on
    stdout/stderr. Standard SF practice; reduces output-deadlock risk
    in pipe scenarios.

Cumulative session A/B (30 games, Threads=1 both sides):
**+82.6 ELO over lmrhist baseline** (W=11/D=15/L=4 = 61.7% score, 95%
CI [-40, +231]). Wide CI, but point estimate strongly positive.

---

## Known issues / diagnostics from game-mining tools

PGN profiling (`testing/game_profile.py`) on 640 games:
- **Time management**: well-calibrated. Δ between W/D/L per-move time is
  <17 ms in every game phase. No issue.
- **Opening weakness**: C-family ECOs score only **34.8%** vs A:45.2% and
  D:50.2%. Worst code: **C45 (Scotch Game): 0W/3D/24L = 5.6%**. 1.Nf3
  scores 42.0% vs 1.d4 49.4% — bot prefers 1.d4 first moves.

NPS profile (`testing/nps_profile.py`) at depth 14:
- Opening / middlegame (23+ pieces): mean **341k NPS**
- Endgame (≤10 pieces): mean **3.1M NPS** — 9× faster
- Cause: NNUE threat features are non-incremental, recomputed each
  forward(); cost scales with active pieces. Stockfish has the same
  pattern but with more SIMD-optimized inference.

**Crash bug** (NOT YET FIXED): Hypersion crashes (ACCESS_VIOLATION,
exit 0xC0000005) via python-chess pipe I/O on positions where major
pieces are close enough to attack each other (Q+Q on adjacent files,
Q vs R, R+R, etc.). Reproducer: `8/4kp2/3p4/3P1q2/8/4Q3/5PK1/8 w - - 0 1`
through python-chess `analyse()`. Manual cmd `go depth 14` on the same
position works fine — only triggered by piped stdin/stdout combined
with these positions. cutechess matches likely lose individual games
to this silently via `-recover`. Probably a thread-shutdown race or
NNUE-threat-buffer issue specific to fully-buffered stdout. Needs
deeper debugging (sanitizers / WinDbg).

---

## Diagnosed weakness — eval scaling miscalibration (NOT YET FIXED)

Stockfish-driven analysis of 240 gauntlet games via python-chess +
Stockfish at depth 16-18 surfaced that Hypersion's losses are **not**
endgame-technique failures. They're driven by a deeper bug:

**Hypersion's NNUE eval is on a different scale than Stockfish's.**

  - Static eval comparison on identical positions, identical NNUE files
    (`nn-c288c895ea92.nnue`, `nn-37f18f62d772.nnue`):
      * Starting position : Stockfish NNUE +0.12, Hypersion +0.59 (~5×)
      * After 1.e4 c5 2.Nf3 Nc6 : Stockfish +0.27, Hypersion +1.37 (~5×)
  - Same-depth search comparison (depth 16) on losing-game positions
    averages ~3.0-3.5× scale ratio.

Hypersion's `search.cpp` already partially compensates via "scaled by 3"
constants (RFP, razoring, futility, SEE, NMP, ProbCut, aspiration,
qsearch — all multiplied by 3). The actual eval scale is closer to ~5×
in static eval / ~3.3× in deep search, so the compensation is approximate
but not precise.

Walk-back analysis (how soon does Hyp's eval diverge from SF ground
truth in lost games):
  - 70% of losses already diverge by **move 11** (right after opening book)
  - 93% with 21+ pieces still on the board
  - Only 3% in deep endgame

This means losses happen because Hypersion plays middlegame moves
believing the position is roughly equal when Stockfish's evaluation says
it's already -100 to -200 cp. By the time Hypersion's search catches up,
the position is unsalvageable.

**Path A attempted, reverted — both variants regressed:**

- **Path A v1** (commit `fd11f39`, reverted in `bf81bbf`): set
  `NNUE_DIVISOR=5` in `nnue.cpp` and restored all `*3`-scaled search
  margins to their bare SF-classical values.
  * Verified: static eval matched SF exactly (11 cp / 27 cp on probe
    positions).
  * Bench shot up 648k → 993k (+53% nodes per fixed depth) because
    bare SF margins are 1/3 of lmrhist's empirically-tuned values
    on Hypersion's specific search architecture.
  * 200g A/B vs lmrhist: **-29.6 ELO** (CI [-79, +19]). At fixed TC,
    +53% nodes per move = significantly less effective depth → loss.

- **Path A v2** (commit `145ccc5`, reverted in `c0d4ee1`): kept
  `NNUE_DIVISOR=5` but rescaled margins to `lmrhist / 5` to preserve
  margin/eval ratio.
  * Bench dropped to 611k (close to lmrhist's 648k).
  * 30g A/B vs lmrhist: **-170 ELO at 13 games before stop** (W=2 D=3
    L=8). Games were also abnormally slow — only 13 games in 17 min
    vs typical 30-game match in ~6 min. Some subtle interaction with
    the integer-truncated rescale produced both lost games and
    timeouts that bench couldn't detect.

**Lesson**: Hypersion's empirically-tuned search constants depend on
its specific eval magnitudes in ways that don't trivially proportionate.
Mathematically "correct" rescaling broke real-world performance. The
5x-scaled eval display is cosmetic; internal pruning behavior was
already self-consistent. Save the eval-scale fix for a future major
retuning effort with broad fishtest-style coverage.

Game-analysis tooling lives under `testing/`:
- `analyze_blunders.ps1` — fast PGN-comment scan (no Stockfish needed)
- `stockfish_blunder_check.py` — depth-18 ground truth via python-chess
- `stockfish_divergence_check.py` — finds the move where Hypersion's
  eval first diverges from SF ground truth
- `stockfish_vs_hypersion_eval.py` — same-depth side-by-side comparison

Tested-but-reverted (logged so they don't get retried blind):
- Worsening flag in LMR (eyeballed magnitudes)
- Score-drop time extension (eyeballed magnitudes)
- History decay on `ucinewgame` (halve instead of zero)
- `Move Overhead` default 30 → 100 ms (ate the 10+0.1 increment, −26 ELO)
- `UCI_Variant` combo (broke python-chess / lichess-bot)
- Lynx 3/4 history gravity (−35 ELO, double-decays our soft-cap update)
- Lynx threats-aware butterfly history (−61 ELO, sparse table without
  compensating bonus tuning — 4× dilution per slot)
- **Lynx score-stability + 5-bucket bm-stab + bonus/malus split** (the
  trio above formerly listed as items 10-12). Each individual A/B used a
  40-game sample (CI ±90 ELO) and looked positive (+26 / +80 / +52). The
  cumulative 200-game match against v1.0 measured **−12.2 ELO** (CI
  [−61, +36]) — statistically flat, point estimate negative. The earlier
  "+158 ELO" cumulative reading was almost certainly noise. **Lesson:
  never validate an engine change at fewer than 200 games.**

---

## Hypersion 2 (deprecated — never publicly released)

A v2.0 tag and GitHub release briefly existed (2026-05-03) but were
deleted. The +17 ELO over v1 was within statistical noise and not enough
to warrant a casual-player-facing release under the new policy. Code
changes that were in v2 are now part of "Unreleased" above.

---


---

## Hypersion 1 (2026-05-02, [release](https://github.com/RenCopp/Hypersion/releases/tag/v1.0))

First public release.

- SF18 SFNNv10 NNUE inference (HalfKAv2_hm + FullThreats, big 1024-d FT,
  small 128-d FT, 8 PSQT buckets, 8 layer stacks).
- Incremental accumulator updates with king-move full-refresh trigger.
- Finny refresh cache (per-(king-bucket, orient, color) accumulator
  cache, ~333 KB per thread) for faster king-move full refresh.
- AVX2 SIMD primitives plus an opt-in AVX-VNNI dpbusd path
  (`ARCH=x86-64-avxvnni`).
- NNUE eval scaling: raw NNUE in SF18 cp magnitude (no `/N` divisor),
  search margins (futility / razoring / aspiration / ProbCut / SEE) all
  scaled to match.
- Easy-move time scaling (`d ≥ 6` + 2nd-best gap + stable iters → cut
  optimum time when bestmove is dominant).
- SF-style small/big network switching with `|small_eval| < SMALL_FALLBACK_TH`
  fallback to the big network.
- Corrected `use_small()` to use Hypersion's SF-style PieceValue scale
  (matching SF's 962 cp threshold).
- Corr-history `__builtin_prefetch` after every `do_move` / `do_null_move`.

Tested at +360 ELO over the engine's pre-NNUE classical baseline at
TC 10+0.1 (40-game matches), and +52 ELO at TC 60+0.6 (20 games).
