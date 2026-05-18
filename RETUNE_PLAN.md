# Hypersion comprehensive retuning plan — INVALIDATED 2026-05-18

## Update — plan abandoned

**Status: ABANDONED.** Tier 3 v2 SPSA at nodes=500000 produced extreme TC-mismatch:
- Bullet 5+0.05 200g: **+284.9 +/- 53.2 ELO** (148W-13L-39D) — provisional SHIP
- LTC    60+0.6 200g: **-197.9 +/- 52.2 ELO** (37W-140L-23D) — CATASTROPHIC

**Net 483 ELO TC swing — invalidates the central premise of this plan.**
nodes=500000 SPSA finds values that optimize for **bullet-depth move
ordering** but **actively hurt** deep search. The remedy isn't "tune at
even higher nodes" — the SPSA gradient at any node count above search-
time depth produces bullet-biased optima.

Real fix paths (not in this plan, future work):
1. **TC-mode SPSA at target TC** (`--tc 60+0.6 --concurrency 6`):
   slow (~6-10h per campaign), but exact-gradient match to LTC tests.
2. **Per-TC parameter tables** + runtime gating (like Tier 2 v2's
   useThreatHist flag): bullet vs LTC values selected by tm.optimum.
3. **Accept bullet-only tuning**: ship a "Hypersion-bullet" variant
   alongside the LTC engine.

Tier 3 v2 has been reverted in `src/search.cpp::tunables`.

---

## Original plan (preserved for reference)

**Original trigger**: Tier 3 v2 SPSA at nodes=500000 shipped at **+284.9 ELO @ 200g**, exactly where Tier 3 v1 at nodes=50000 had tombstoned at -34.9 ELO. The (now-disproven) lesson: **every prior tombstone with "SPSA REJECTED" is suspect** because they all used `nodes=50000` (Hypersion's `testing/spsa.py` default). Re-running them at `nodes=500000` may unlock substantial latent ELO.

CLAUDE.md TC-specificity finding (2026-05-09) is the foundational evidence:
> SPSA campaigns ran at nodes=50000 (bullet-equivalent depth ~10-12). The local optima found at that depth don't necessarily generalize to the deeper search trees (depth 25+) that LTC produces.
> For LTC-specific tuning, run SPSA at nodes=500000+ or use TC-mode with tc=60+0.6.

## Hypersion's full SPSA-tunable parameter inventory

Identified by grepping `set_tunable()` in `src/search.cpp`. **34 parameters** across 8 groups.

### Group P1 — Pruning margins (7 params, NNUE-strongly-masked)
- `RFP_MARGIN_PER_DEPTH`     (def: 240)
- `RAZOR_MARGIN_BASE`        (def: ?)
- `RAZOR_MARGIN_PER_DEPTH`   (def: ?)
- `FUTIL_MARGIN_PER_DEPTH`   (def: 397)
- `FUTIL_MARGIN_BASE`        (def: 385)
- `NMP_EVAL_BETA_DIV`        (def: ?)
- `PROBCUT_MARGIN`           (def: ?)

Prior result at nodes=50000: tombstoned. Per research, NNUE-strongly-masks static-eval pruning, so even nodes=500000 may yield neutral. But the magnitude of unlock could be substantial if it works (these are 7 of the most-impactful constants in the engine).

### Group P2 — SEE margins (2 params)
- `SEE_QUIET_MARGIN`         (def: -181)
- `SEE_CAPT_MARGIN`          (def: -252)

NOT NNUE-masked (SEE is move-ordering / pruning gate based on material exchange, not eval). High retest priority.

### Group P3 — Aspiration window (3 params)
- `ASPIRATION_DELTA0`        (initial window size)
- `ASP_GROWTH_ADD`           (growth constant)
- `ASP_FULL_WINDOW_TH`       (fall-back threshold)

Affects search structure but not eval pipeline. Medium priority.

### Group P4 — Stability/effort time scaling (5 params)
- `STABILITY_SWING_TH`       (def: 61)
- `FALLING_EVAL_DIV`
- `EFFORT_TH`
- `EFFORT_SCALE`
- `STABLE_HIGH_SCALE`, `STABLE_LOW_SCALE`

Time-control-sensitive — SPSA at higher nodes should map better to these.

### Group P5 — Time management scales (6 params, A4 campaign)
- `TM_ENDGAME_BONUS_8`, `_12`, `_16`
- `TM_EASY_GAP150`, `_80`, `_40`

Prior result at nodes=50000: -3.5 ELO (near-zero noise). Could swing either way at nodes=500000.

### Group P6 — Malus split (4 params)
- `HIST_MALUS_DEPTH2`, `HIST_MALUS_DEPTH1`, `HIST_MALUS_CAP`, `HIST_MALUS_CONST`

Prior result at nodes=50000: -19.1 ELO. SPSA found small shifts that hurt. Defaults bit-match bonus formula. Retry at nodes=500000.

### Group P7 — Eval mixing (3 params, NNUE-related)
- `PSQT_WEIGHT`, `POSITIONAL_WEIGHT`, `MATERIAL_SCALE_BASE`

Affects how NNUE psqt + positional buckets blend. **High risk** — changing eval weights for the shipping network is exactly the kind of thing NNUE-masking research warns against. **LOWEST priority** — skip unless other groups succeed.

### Group P8 — Misc tunables (4 params)
- `HIST_MAX`                 (gravity divisor)
- `LMR_STATSCORE_DIV`        (def: 7938)
- `QSEARCH_CAP_GAIN`         (def: 3221)
- `BFLY_WEIGHT`              (def: 101)

History-formula tweaks. Some prior single-param sweeps tombstoned.

## Execution plan

Sequential SPSA campaigns at `nodes=500000`, conc=6, `--iters 200 --games-per-iter 4`. Each ~3-4 hours wall-clock with full CPU. Total **~25-30 hours** for all 7 groups (P7 skipped as too risky).

| Order | Group | Params | Expected wall-clock | Confidence |
|---|---|---|---|---|
| 1 | P2 SEE margins | 2 | ~3 h | HIGH (move-ordering-like, similar to Tier 3 v2) |
| 2 | P6 Malus split | 4 | ~3 h | MEDIUM (already had nodes=50000 fail; retry at higher) |
| 3 | P1 Pruning margins | 7 | ~4 h | MEDIUM (NNUE-masked but 7 params = most leverage) |
| 4 | P4 Stability/effort | 5 | ~3 h | MEDIUM-LOW (TC-sensitive, may not help across TCs) |
| 5 | P3 Aspiration | 3 | ~3 h | LOW (structural search, not magnitude tuning) |
| 6 | P5 Time mgmt scales | 6 | ~3 h | LOW (-3.5 ELO at nodes=50000 — marginal retest) |
| 7 | P8 Misc | 4 | ~3 h | LOW (mixed-category catchall) |
| (skip) | P7 Eval mixing | 3 | — | TOO RISKY (NNUE-net-coupled) |

Total: 7 campaigns × ~3.3 h average = **~23 hours wall-clock**, plus SPRT validation (~30 min each = 3.5 h) = **~27 hours total**.

## Per-group workflow

For each group:
1. **Build params JSON** at `testing/spsa_params_<group>.json` with appropriate min/max/step/learn ranges around current defaults
2. **Snapshot** current `Hypersion.exe` as the SPSA base binary (so the SPSA runs against current-shipped values, not stale)
3. **Run SPSA** at `nodes=500000 --concurrency 6 --games-per-iter 4 --iters 200`
4. **Apply** converged values to `src/search.cpp::tunables` defaults
5. **Build candidate** + snapshot
6. **SPRT triage** 30g at TC 5+0.05 conc=6
   - If ≤ +50 ELO 30g (NOISE band) → **proceed to Stage 2** (per "run all" directive)
   - If clearly negative (< -50) → revert + tombstone, skip to next group
7. **SPRT confirm** 200g at TC 5+0.05 conc=6
   - If > +10 ELO with CI lower bound > 0 → **SHIP** the values
   - If ≤ +5 with CI ±35 → **REJECT**, revert values, tombstone with measurement
8. **Optional LTC validation** 200g at TC 60+0.6 if Stage 2 ships big (caution about TC-specificity)

## Gates / stop-loss

- **STOP early** if 3 consecutive groups REJECT — strong evidence local optimum is genuinely tight, further tuning is wasted CPU
- **PAUSE for human review** after every 2 ships (consecutive +30+ ELO each) — anomalously high success rate warrants validation

## Final deliverable

A **v3.2 release** containing all retuned values that pass SPRT, with:
- AUDIT.md updated with all measurements
- Per-group tombstones for any failures
- Final 6-engine tournament showing cumulative gap closure vs reference engines
- Source comments citing per-parameter SPRT result with ELO ± CI

## Risk register

1. **Compound TC-specificity**: nodes=500000 matches bullet TC depth well but may NOT transfer to LTC (60+0.6 reaches depth 22-27). Some retuned values may still bullet:LTC-mismatch. Mitigation: LTC-validate any big-ship result.

2. **CPU wear**: 27 hours of continuous heavy CPU usage. Mitigation: monitor temperatures, allow cooldown breaks if needed.

3. **Disk space**: each SPSA campaign generates ~5-10 MB of progress logs + PGN. Total ~70 MB. Mitigation: trivial.

4. **Bench non-determinism**: if SPSA runs on a binary with stale .o files, may produce phantom signal. Mitigation: `make clean && make -j` before each campaign's binary snapshot.

5. **Joint-tune dependencies**: some groups may need EACH OTHER tuned jointly to find global optima. P1+P2+P4 could be co-optimized in a 14-param mega-campaign. Mitigation: if sequential gives 0 ships, try the mega-campaign as final stand.

## Ordering rationale

P2 first because: (a) 2 params = fastest, (b) SEE is move-ordering-adjacent like Tier 3 v2's winning group, (c) clear failure semantics if it doesn't work (small param count, easy to interpret).

P6 second because: already had a nodes=50000 SPSA failure to compare against, validates the methodology.

P1 third because: highest absolute parameter count (7), biggest potential ELO swing if it works.

P4/P3 mid because: structural-but-not-magnitude-tuning, medium confidence.

P5 last (of attempted) because: lowest signal at nodes=50000 (-3.5).

P7 skipped: NNUE-coupled, too risky to tune for shipping network.
