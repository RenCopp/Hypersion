# Hypersion CHANGELOG

## Session 2026-05-15 (extended) — first-move tombstones, LMR ship, interior sweeps

18 commits, all green CI. **Engine ELO shipped: +20.9 ± 39.3** via
LMR divisor tweak (v18, e9486d7). Plus 7 additional tombstones across
2 thematic groups.

### SHIPPED
- **v18** (`e9486d7`) — LMR divisor `1.85 -> 1.87`. SPRT 200g at TC
  5+0.05 conc=6: **+20.9 ± 39.3 ELO** (72W-60L-68D, NEW-as-White
  54.6 %, NEW-as-Black 50.5 %, symmetric). The 1.87 sweep point
  lives in the previously-unexplored interior between SF18 master
  (1.85) and Hypersion v2.0 (1.90). Smaller divisor = larger
  reduction; 1.87 means slightly SHALLOWER cuts than 1.85.

### Group 1: first-own-search parameter tombstones (3 rejects, both directions)
- **v8 H1** — +30 % optimum-time budget for moves 1-3.
  SPRT 200g: -24.4 ± 39.3 ELO. Tombstoned in `src/timeman.cpp`.
- **v13** — 4x wider initial aspiration window for first own move.
  SPRT 200g: -10.4 ± 38.3 ELO. Tombstoned in `src/search.cpp`.
- **v16** — REDUCE optimum-time budget -23 % for moves 1-3 (inverse
  of v8 H1). SPRT 200g: -15.6 ± 38.2 ELO. Tombstoned in `src/timeman.cpp`.

Both more-effort variants (v8 H1, v13) showed strong Black asymmetry
(NEW-as-Black ~39 %). v16 (less effort) had no asymmetry but still
regressed ~15 ELO. Combined verdict: first-move time/window settings
are at a tight local optimum at TC 5+0.05. Anti-pattern documented
in `.claude/skills/chess-engine-dev/references/common-bugs.md`.

### Group 2: SPSA-tunable interior sweep points (4 rejects, 1 ship)
- **v18** SHIPPED — see above.
- **v19 + v20** — LMR sweep continuation: 1.87 -> 1.89 = 0.0 ELO @
  30g (11W-11L-8D); 1.87 -> 1.86 = 0.0 ELO @ 30g (10W-10L-10D).
  1.87 confirmed as local optimum.
- **v21** — `ASPIRATION_DELTA0` 50 -> 65. -34.9 ELO @ 30g (8W-11L-11D).
- **v22** — `RAZOR_MARGIN_BASE` 852 -> 750. -8.7 ± 37.9 ELO @ 200g
  (after +11.6 fakeout at 30g).
- **v23** — `SEE_QUIET_MARGIN` -181 -> -200. -15.6 ± 37.2 ELO @ 200g
  (after +23.2 fakeout at 30g; Black asymmetry 40.9 %).

Lesson: interior-sweep-point interpolation works when BOTH endpoints
showed positive sweep results vs prior baseline (LMR 1.85 + 1.90
both positive vs 1.95). When one endpoint is strongly negative
(SEE_QUIET -150 = -70 ELO), the interior doesn't help.

### Infrastructure (unchanged from earlier summary)
- **Build**: `-MMD -MP` automatic dependency tracking (`95a3359`).
- **CLAUDE.md**: bench non-determinism note (`a80310e`).
- **.gitignore**: `release/`, `*.bak`, `*.exe.stripped` (`c9fe6db`).
- **README**: Build / CodeQL / GPLv3 badges (`1f865a0`).
- **Skill**: anti-pattern doc (`0f082b0`, updated in `a365636`).

## Session 2026-05-15 — anti-patterns + infrastructure cleanup (early phase)

[Note: superseded by the extended session entry above which includes
the v18 ship and post-v8/v13/v16 LMR + interior-sweep work.]

10 commits, all green CI. No engine ELO shipped — confirmed three
parallel rejections at the first-own-search parameter granularity
(both directions tested), and consolidated build / docs.

### Engine experiments (all THREE REJECTED, tombstoned)
- **v8 H1** — boost optimum-time budget +30 % for the first 3 own
  searches per game. Hypothesis: TT-cold searches need more time.
  SPRT 200g TC 5+0.05 conc=6: **-24.4 +/- 39.3 ELO** (59W-73L-68D).
  Tombstoned in `src/timeman.cpp`. Field `SearchLimits::ownSearchIndex`
  + uci.cpp counter kept for future variants.

- **v13** — widen initial aspiration window 4x (delta=200) for the
  first own search. Hypothesis: TT-cold prevScore is unreliable, so
  the standard +/-50 window fails repeatedly. SPRT 200g TC 5+0.05
  conc=6: **-10.4 +/- 38.3 ELO** (60W-66L-74D). Tombstoned in
  `src/search.cpp`.

- **v16** — REDUCE optimum-time budget -23 % for the first 3 own
  searches (opposite-direction test of v8 H1). Hypothesis: less
  effort on first move might bypass the Black-side asymmetry seen
  in v8 H1 / v13, and saved clock flows to moves 8-30 where the
  actual blunders cluster. SPRT 200g TC 5+0.05 conc=6: **-15.6
  +/- 38.2 ELO** (58W-67L-75D). Classic 30g-fakeout pattern
  (Stage 1 was +70 ELO at 30g). Tombstoned in `src/timeman.cpp`.

v8 H1 + v13 (more-effort variants) both showed a Black-side asymmetry
(candidate-as-Black ~39 %, candidate-as-White ~57 %). v16 had no
asymmetry — both sides under-parity ~3-5 ELO. **Combined verdict**:
first-move time/window settings are at a tight local optimum at TC
5+0.05; neither direction helps. Anti-pattern doc in
`.claude/skills/chess-engine-dev/references/common-bugs.md`.

### Infrastructure (all green CI)
- **Build**: `-MMD -MP` automatic dependency tracking. Editing a
  header now correctly rebuilds all dependent .o files; eliminates
  the spurious `-Wodr` warning from incremental builds with LTO.
- **CLAUDE.md**: bench non-determinism was previously documented as
  "RESOLVED 2026-05-13"; re-measured at Threads=1 and found 14-30 %
  variance across processes. Moved back to open status with
  candidate causes and a 5-run sanity check replacing the previous
  exact-match `make verify` gate.
- **.gitignore**: `release/`, `*.bak`, `*.exe.stripped` so the
  working tree stays clean after packaging or tuner runs.
- **README**: Build / CodeQL / GPLv3 badges.

### Black-vs-1.e4 blunder analysis (deferred reference)
- `testing/analyze_blunders.py`: Stockfish 18 at depth 14 over the
  6 Hypersion-as-Black-vs-1.e4 lichess losses. Persistent SF UCI
  process; ~60s wall-clock for the full sweep.
- `testing/BLUNDER_ANALYSIS.md`: actual non-mate blunders cluster
  at moves 8-30, NOT 1-3 — invalidates the "empty TT" hypothesis
  that drove v8 H1 / v13. (Both files live under gitignored
  `testing/` tree; check the repo at the user's machine.)

## Project status (2026-05-14)

**Classical eval expansion + tuning session complete.** Engine
classical-eval has been broadened from 14 to 145 tunable scalars
across 16 feature rounds (R1-R37). Best WAC depth-8 result of the
project: 184/198 (92.9%). NNUE-shipped path is unchanged
(1,273,328 nodes T1 d=13).

Single-feature search-port retries from the `history.h` tombstone
register continue to regress. Future progress requires:
- Joint SPSA over coordinated parameter clusters (validated by the
  R22-R31 `--new-only` joint tune — gave WAC +3 while individual
  `--init-only` tunes regressed)
- LTC self-play datasets for Texel (tactically aligned vs master games)
- WDL-aware Texel loss (research-grade work)
- NNUE retrain (the original "paused" item, still unaddressed)

## Session 2026-05-14 — Classical eval R20-R37 + 16M Texel re-tune

Commits: `3bbe494`, `a611c61`, `fd7e3c2`.

### New features (active in shipped binary)
- **R20 Mop-up eval**: drive lone king to corner in KX-K endgames.
- **R21 OCB scaling**: 0.5x eg in opposite-colour-bishop endings.
- **R22 Initiative bonus (SF7-style)**: complexity-weighted eg bonus.
  Tuned via `--new-only` 16M joint tune (Outflanking=16, PawnCount=28,
  BothFlanks=40, PureEndgame=100, Offset=0, Scale=112). The widened-
  ceiling re-tune (Outflanking=7, Scale=175) regressed WAC -3 despite
  -0.000061 MSE improvement — same MSE-vs-WAC mismatch as R32-R37.
- **R23 KBNK mating drive**: PushToCorners + PushClose by bishop colour.
- **R25 Drawish endgame scaling**: wrong-bishop+rook-pawn 20%, KNP 50%.
- **R26 KPK bitbase** (`src/kpk_bitbase.h`, new): 24KB retrograde-built
  lookup table for all 98,304 K+P-vs-K positions. Built at startup
  via `Eval::init()`. Probed in classical eval to scale eg for known
  drawn positions.
- **R27-R31** (joint-tuned with R22): KnightRimPenalty, PawnIslandPenalty,
  BishopPairOpenScale, RookOn8thEG, QueenKingTropismMG.

### Features in code but DISABLED in shipping binary
R32-R37 implemented and tuned, but tuned values regress WAC depth-8
by 3-8 points (the MSE-vs-WAC mismatch). Disabled at 0 for shipping;
code preserved for future LTC-SPRT validation:
- R32 ConnectedPasser (MG/EG), R33 TradeDownBonus,
- R34 BadBishopBlocked (MG/EG), R35 RookTrappedByKing
  (with castling-rights-required detection — fixed bug from initial
  implementation that false-positived normal castled positions),
- R36 BackwardOnHalfOpen (MG/EG),
- R37 Imbalance polynomial (`src/imbalance.h`, new) — SF-style 6x6
  QuadraticOurs+QuadraticTheirs matrices. Enabled at scale=2 (tiny
  activation) in fd7e3c2 since NNUE-shipped bench is unaffected.

### Infrastructure
- `src/kpk_bitbase.h`, `src/imbalance.h`, `src/pawn_hash.h` — new headers.
- `tools/tuner/tuner.cpp`: +500 LOC, 4 new isolation flags
  (`--init-only`, `--scale-only`, `--new-only`, `--part2-only`).

### Verification
- WAC depth-8 classical: **184/198 = 92.9%** (project best)
- NNUE bench T1 d=13: **1,273,328** (unchanged from v3.0 baseline)
- 3 release variants (avx2/avxvnni/bmi2): all 1,273,328 nodes

### Tombstones from this session
- **R22 widened-ceiling re-tune**: MSE -0.000061, WAC -3 (181/198).
  Reverted to Phase-A joint-tune values.
- **R32-R37 master-game-tuned values**: 16M tune found values
  regressing WAC by 3-8 points despite MSE improvement. Tighter
  ceilings (221k retune) still regressed by 3. Master-game MSE
  doesn't preserve depth-8 tactical sharpness; classical "MSE
  optimum != WAC optimum" mismatch.
- **Tactical-aligned tune** (decisive games + WAC-anchored positions
  replicated 50x): 141k positions, MSE -0.001159, WAC -10 (174/198).
  Anchor approach overfits — small dataset can't generalize past
  the 199 WAC positions. Reverted.
- **Stale-obj-files hang** documented in IMPROVEMENTS_LOG: adding
  eval_params.h struct fields requires `make clean` (otherwise
  search hangs at depth 7+ from binary-layout mismatch).

### Bullet TM fixes (commits 74d5b49 + 15d7f2a)

**Root cause**: when `wtime < moveOverhead` (default 2000 ms), the
old TimeManager clamps `remaining = max(wtime - overhead, 1) = 1 ms`,
collapsing optimumTime to MIN_MOVE_TIME_MS = 10 ms. The engine
panics at sub-2-second time with a 10 ms budget per move, often
producing no completed search iteration.

**Fix A** (74d5b49): cap `overhead = min(moveOverhead, wtime/2)` so
the engine always has at least half its remaining time after overhead
subtraction.

**Fix B** (15d7f2a): SF18-style mtg scaling — when remaining < 1 sec,
reduce moves-to-go horizon proportionally:
`mtg = max(2, remaining/20)`. At remaining=500ms, mtg=25 instead of 40;
at remaining=100ms, mtg=5. Gives more time per move when time is low.

**Validation** (testing/test_bullet_conversion.py):
| Scenario | wtime | Before | After |
|---|---:|---:|---:|
| K+R+P vs K+P | 1500ms | 1207ms (80%) | **184ms (12%)** |
| K+R vs K | 500ms | 181ms (36%) | 182ms |
| Passed pawn EG | 1000ms | 182ms (18%) | 265ms |
| K+Q vs K | 500ms | hangs | hangs (separate bug) |

The K+R+P case (most common winning bullet position) now uses 85%
LESS time. Should significantly reduce bullet flag-outs in lichess
games where the bot has a winning conversion.

### PGO release builds (2026-05-14)

Built PGO variants for all 3 architectures using the project's
existing `make profile` target. Profile data generated by running
`bench 13` and `bench 11` on the instrumented binary, then re-built
with `-fprofile-use`. NPS gains:

| Variant | Non-PGO NPS | PGO NPS | Gain |
|---|---:|---:|---:|
| avx2 | 428,297 | 443,822 | +3.6% |
| avxvnni | 428,297 | 447,252 | +4.4% |
| bmi2 | ~430,000 | 449,304 | +4.5% |

Bench unchanged at 1,273,328 nodes across all variants (PGO doesn't
change search behavior — same nodes, just faster execution).

PGO variants are stored separately:
- `release/Hypersion-x86-64-avx2-pgo.exe`
- `release/Hypersion-x86-64-avxvnni-pgo.exe`
- `release/Hypersion-x86-64-bmi2-pgo.exe`

**Deployment guidance** (per Makefile comments):
- Use PGO build for **single-instance production** (lichess-bot,
  analysis tools). Expected ELO gain: +5-10 from deeper search at
  same time budget.
- Do NOT use PGO for SPRT testing or high-concurrency tournaments
  — i-cache pressure causes PGO to regress at concurrency>=4
  (tested: -45.8 ELO @ 61g, concurrency=6).

To deploy in lichess-bot, change `config-hypersion.yml`:
```
name: "Hypersion-x86-64-avxvnni-pgo.exe"   # was avxvnni
```

### T1.1 Material-aware draw contempt — TOMBSTONED (2026-05-14)

Tried adding material-aware contempt to `value_draw()`: when STM is
materially ahead/behind by >400 cp, adjust the returned draw value
by up to ±50 cp so the search avoids settling for repetition when
winning (or accepts draw when losing).

**Result**:
- NNUE bench: 1,273,328 → 1,273,315 (-13 nodes, breaks deterministic
  parity)
- WAC depth-8: 184 → 182 (-2)

The change is small but non-zero. Search apparently relies on the
exact draw value for alpha-beta window calculations; material-driven
shifts subtly change pruning behavior. Reverted.

Future contributor: re-try with much smaller penalty (≤5 cp), or
gate behind `limits.contempt > 0` only (user-explicit anti-draw mode).

### Cross-session opening variety (commit f31f6a5)

Ported from Kirin V8's `OpeningBook` recent-moves filter. The bot now
tracks its last 16 first-moves played at the starting position across
sessions in `hypersion_recent_openings.txt`. When probing the book at
startpos, recently-played first moves are excluded from candidates,
with progressive window shrink (8→7→...→1) if all candidates would be
excluded. The chosen move is appended to the state file.

**Effect**: bot rotates through e4/d4/c4/Nf3 etc. instead of always
playing the highest-weighted move. Test (5 consecutive `position
startpos; go depth 1` runs from a fresh state file): chose g1f3,
c2c4, d2d4, e2e4, g1f3 — all distinct in the first 4 runs.

State file is created next to the executable. Lichess-bot can delete
it to reset variety.

Originally credit: Kirin V8 (C:\Engine\Kirin V8\kirin_engine.py
OpeningBook._load_recent / _remember_first_move). Most of Kirin's
features were already in Hypersion (per Kirin's own
IMPROVEMENT_PLAN.md which ports FROM Hypersion); this was the one
genuinely-Hypersion-doesn't-have feature.

### KQK Syzygy hang FIXED (commit b6e44df)

Position `8/8/8/4k3/8/8/8/Q3K3 w - - 0 1` (K-Q on e1/a1 vs K on e5)
hung the engine when Syzygy was loaded. Root cause: Fathom's
`tb_probe_root_dtz` for KX-vs-K positions where kings are on the
same file recurses uncontrollably through `probe_dtz` -> table miss
-> `probe_dtz_table` returns success<0 -> generate all moves and
recurse on each, which itself returns success<0 etc.

Without Syzygy, search resolves the position to depth 21 mate-in-12
in <1 second. With Syzygy active, the engine hung indefinitely on
this specific configuration. Other KQK positions (e.g., Qa2/Ke1 vs
Kd6 or Ke8) probed fine.

**Workaround**: skip root DTZ probe for positions with <=4 pieces
and no pawns (the trivial-conversion zone where the regular search
finds the win quickly anyway). This loses the DTZ root-rank
optimization for 3-4 piece positions but preserves the engine's
search reliability. WDL probes at internal nodes still work
(search.cpp:1711) — those use a simple table lookup with no
recursion.

After fix:
- All 4 test_bullet_conversion scenarios pass (was 3/4)
- Bench unchanged at 1,273,328
- WAC unchanged at 184/198

## Project status (2026-05-11) — previous

## Post-v3.0 session (2026-05-10/11)

Targeted session on a user-reported endgame bug, plus follow-up
investigations into NNUE performance, LTC tuning, and tactical
analysis-mode pruning.

### Shipped

- **Endgame mate-conversion fix** (commit eb35855) — user-reported
  bug at lichess H05vgtVr: Hypersion @ UCI_Elo ~1500 drew K+R vs K
  by 50-move rule. Root cause: strength limiter caps nodes hard
  (800 nodes/move @ UCI_Elo=1500), too tight for a 16-ply K+R+K
  mating plan. Stockfish handles the same case via MultiPV+pick_best
  (only modifies move SELECTION at one specific depth, never caps
  nodes). Fix: in `clearlyWinningEndgame` (popcount<=10 AND material
  lead >= rook), fully disable strength limiting. Plus `is_shuffling()`
  helper ported from SF master post-SF18.
  Validation: 245/252 = **97% conversion** across 21 cells
  (7 UCI_Elo levels x 3 movetimes x 12 positions). Stockfish full-
  strength sanity 12/12; Stockfish at UCI_Elo=1500 handicap 8/12.
  No regression at full strength (override gated on applyEloCaps).

- **Real UCI_AnalyseMode pruning relaxation** (commit 26fa62d) —
  previously the flag only disabled the opening book. Now it also:
  disables NMP (zugzwang false-positives can hide mate threats),
  skips Late-Move-Pruning, skips shallow-depth pruning (futility +
  SEE-quiet/capture margins), and halves LMR aggression. Tactical
  suite uplift at standard wac_runner conditions (which sets
  UCI_AnalyseMode=true):

  | Suite      | Before | After  | Delta  |
  |---|---|---|---|
  | WAC        | 93.9%  | 96.5%  | +2.6%  |
  | mate-in-3  | 94.5%  | 97.5%  | +3.0%  |
  | mate-in-5  | 85.4%  | 88.9%  | +3.5%  |
  | mate-in-8  | 65.0%  | 66.0%  | +1.0%  |

  Default play (UCI_AnalyseMode=false) is bit-for-bit unchanged —
  verified by `bench 13 @ Threads=1` producing exactly 1,107,765
  nodes on both old and new binary.

### Investigated but tombstoned

- **NNUE small-only mode** (commit 635e2ce, infrastructure kept) —
  -47 +/- ~40 ELO @ 96/100g LTC. Multiple engine disconnects suggest
  stability issues with the small-only path under match conditions.
  Small net's per-position eval accuracy too low for the ELO budget
  to recover from the +3-5x NPS gain. UCI option `EvalUseSmallOnly`
  default false; flippable for future re-experimentation.
- **A4/A6 LTC SPSA retry** (commit d192f25) — joint LTC SPSA on the
  same 8 time-mgmt + history-gravity tunables that bullet SPSA had
  already tombstoned. Larger parameter shifts than bullet (6-27%
  vs bullet's <4%), but 100g LTC SPRT confirmed **0.0 +/- 53 ELO**
  — perfectly neutral. These knobs are at local optima at all TCs.
- **NNUE threats network ablation** (source-only, reverted) —
  refresh_threats path is 70% of NNUE inference time
  (350k -> 1.17M NPS without it = 3.35x speedup). SPRT NoThreats vs
  WithThreats:
  - Bullet 5+0.05 100g: **+3.5 +/- 54 ELO** (neutral)
  - LTC 60+0.6 50g:     **-28 +/- 81 ELO** (marginally negative)

  Speedup and eval-quality loss nearly cancel at bullet; threats win
  marginally at LTC. Not enough leverage to justify shipping the
  simpler NoThreats version. Threats stay.

### NNUE TC-dependence finding (measurement, no code change)

Phase 5 measurement of NNUE-on vs NNUE-off:

| TC | NoNNUE vs WithNNUE | Notes |
|---|---|---|
| 5+0.05 bullet | +45 +/- 55 ELO | NNUE costs ~45 ELO at bullet |
| 60+0.6 LTC   | -58 +/- 102 ELO | NNUE adds ~58 ELO at LTC |

NNUE inference is **8x slower than classical** in this engine
(2.8M NPS classical vs 350k NPS NNUE @ depth 13). At bullet the
depth advantage of classical eval nearly compensates for the
eval-quality loss. NNUE stays on by default (Hypersion's target
deployment is LTC/lichess-bot). Phase 6 retrain/arch optimization
remains the leverage point if pursued.

---

If you want to contribute an NNUE retrain, see `docs/NNUE.md` for the
current network architecture (SF18 SFNNv10) and open an issue or PR.

## Release policy

Don't tag a new GitHub release until **at least 8 verified improvements**
have stacked on `main` AND a head-to-head A/B match against the previous
release shows **non-regressing strength** (≥ −10 ELO, ideally ≥ +20).
Bigger ELO swings (+50, +200) are bonus targets, not gates.

This keeps releases meaningful for casual users who download the zip,
without holding shipping hostage to multi-month ELO grinds. Daily dev
work stays on `main` (no release needed).

---

## v3.0 (2026-05-09)

**Final development release before NNUE-contributor pause.** Cumulative
+178 ELO point estimate over v2 at bullet 5+0.05; ~+17 ELO at long TC
60+0.6 (CI ±38 — positive but not statistically conclusive). The
session yielded 6 SPRT-confirmed shipped improvements + 2 user-facing
UCI features.

### Shipped improvements (6 ships, bullet 5+0.05)

| Change | ELO @ 200-600g | Mechanism |
|---|---|---|
| Bullet flag-out fix | +22.6 ± 37 (200g) | Skip volatility bump + tighten easy-move scaling at < 2 s remaining |
| A2-v2 history weights | +27.9 ± 17 (400g) | SPSA-tuned BFLY/CONT1/CONT2 multipliers + bonus-formula coefficients |
| A3 search margins | +33.1 95 % CI (+11.9, +54.6) (600g) | SPSA-tuned RFP/RAZOR/FUTIL/SEE/NMP/PROBCUT/etc. (12 params, all <2 % shift, joint effect significant) |
| A5 LMR-statScore | +38.4 95 % CI (+16.7, +61.1) (600g) | SPSA-tuned divisor 8192 → 8063 (single -1.6 % shift, +38 ELO) |
| A9 joint-cluster retune | +28.8 95 % CI (+3.5, +54.7) (400g) | Joint SPSA over the 6 biggest movers — surfaced cross-cluster interaction effects |
| A10 falling-eval-divisor | +27.2 95 % CI (+1.4, +53.0) (400g) | SPSA-tuned per-iter time-bump divisor 1000 → 1058 (-5.8 % shift) |

### New UCI features

- **`UCI_GameTournament`** — when true (set by lichess-bot for arena/swiss
  games), opponent-ELO matching applies even in rated games. Lets users
  run rated tournaments with rating-balanced bot play. lichess-bot
  reads `game.source` field and flips this automatically.
- **Weaker offset curve** — opponent-matching curve shifted -50 ELO
  across all rating bands. The bot now plays consistently 50 ELO below
  the matched target (was 25-100 below). Examples: vs 1600 opp targets
  1450 (was 1500); vs 2200 targets 2100 (was 2150); vs masters
  targets 2650 (was 2700, exact).

### SPSA-tunable infrastructure shipped (35 params total)

All previously-hardcoded magic numbers now exposed as runtime UCI
`Tune_<NAME>` options for future tuning campaigns. Even the params
that didn't move (A6/A7/A8/A11 zero-or-near-zero convergence) stay
exposed because the infrastructure has zero NPS cost when defaults
match. Specifically:

- 12 search-margin tunables (A3 SPSA-tuned defaults)
- 6 history-weight + bonus-formula tunables (A2-v2 SPSA-tuned defaults)
- 3 LMR + threat-by-lesser tunables (A5 SPSA-tuned where it moved)
- 6 time-mgmt scale tunables (A4 tombstoned; pre-A4 defaults preserved)
- 2 history gravity tunables (A6 zero-movement; SF defaults preserved)
- 2 aspiration window tunables (A7 tombstoned; SF defaults preserved)
- 3 NNUE eval-mixing tunables (A8 tombstoned; SF defaults preserved)
- 3 falling-eval/effort tunables (A10 SPSA-tuned where it moved)
- 2 stable-iter scales (A11 zero-movement; SF defaults preserved)

### Investigated but reverted/tombstoned this session

Source-inline tombstones with full ELO ± CI for each:

- **Game-workload PGO** — re-tested at conc=2 (memory-aggressive
  protocol) on the post-A3 codebase: -6.9 ± 37.1 ELO @ 200g. Confirmed
  the prior tombstone direction. Kept the `Makefile` tombstone next
  to `-funroll-loops`. Same family of i-cache-pressure issue.
- **A1 12-param SPSA at 4 g/iter** — both slow-TC and fast-nodes
  variants regressed (-75.9 / -8.7 ELO). Diagnosis: 4 games/iter
  noise floor too high. Future SPSA campaigns at 64+ g/iter (the
  v2 methodology) ship cleanly.
- **A4 time-mgmt scales SPSA** — tombstoned at -3.5 ELO @ 200g. Time-
  management scales are at SF-tuned local optima.
- **A6 history-gravity / A7 aspiration / A8 NNUE-eval-mixing /
  A11 stable-iter scales** — all SPSA-converged with zero or near-zero
  movement. SF18-inherited defaults are at Hypersion's local optima too.

### TC-specificity caveat

Session ELO gains compound to +178 at bullet 5+0.05, +17 ± 38 at LTC
60+0.6. Most gains are bullet-specific (the bullet flag-out fix only
fires at <2 s remaining; SPSA at nodes=50000 reflects bullet depth).
See `CLAUDE.md` for full diagnosis. Future contributors targeting LTC
strength should run SPSA at higher node counts.

### Lichess-bot side changes (local-only, not in this repo)

- `lib/model.py`: Game class now exposes `.source` and `.tournament_id`
- `extra_game_handlers.py`: sets `UCI_GameTournament=True` for tournament
  games (`game.source ∈ {"tournament", "swiss"}` or non-empty
  tournament_id)

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

### PGO build experiment

Re-tested `make profile` (after the fix in commit `e9a07e2`):
* PGO build bench median (5 samples): 707,864 NPS
* Non-PGO build:                       697,821 NPS
* Speedup: ~1.4 % — within the bench-run noise floor

PGO builds work correctly but the marginal gain doesn't justify the
~5× longer build time for normal development.  Kept the `profile`
make target available for release packaging when worth it.

### Lichess-bot config polish (off-repo, in user's local install)

In `lichess-bot-master/config-hypersion.yml`:
* `pgn_directory: "C:/Engine/Hypersion/lichess_games"` — saves all
  played games as PGN for offline review
* `pgn_file_grouping: "all"` — single big PGN file
* `offer_draw_score: 0 -> 50` — was effectively "never accept"
  (|score| <= 0 is impossible); now accepts draws when |score|
  <= 50 cp for 10 moves with <= 10 pieces (sane endgame draw)
* Greeting message updated to introduce Hypersion and link to
  the GitHub repo

### Long-TC verification result

200-game match HEAD (post-v2 cumulative) vs the published v2-tag
binary at TC 60+0.6:

* **Result: -8.7 +/- 38.8 ELO**  (62W-71D-67L)
* CI [-86, +69] crosses zero — statistically neutral

Decision: do NOT tag v2.1. The post-v2 commits don't show a clear
gain at long TC large enough to justify a new release, and the
point estimate is mildly negative. Future work should focus on
features that improve long-TC strength (deeper-search accuracy)
rather than fast-TC tuning.

Cross-check at fast TC: HEAD vs v2-tag at 5+0.05 (same 200 games):
**+1.7 +/- 38.2 ELO**. Both regimes within noise of zero.



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
