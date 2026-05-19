# Hypersion v3.2 plan — verified-untried structural ports

## Methodology

Per the v3.1 pattern: structural ports of new information sources WORK. Magnitude tuning doesn't. The first PLAN_V3.2.md was based on wrong assumption (Hypersion already has multi-factor TM, and Phase 2 PawnHistory rejected). This revised plan focuses on truly-untried features verified by re-reading source.

## Verified untried features (after the deep audit + grep verification)

| Phase | Source | Mechanism | Why untried | Risk |
|---|---|---|---|---|
| T1 | Berserk `search.c:81-83` | `AdjustEvalOnFMR`: `eval = (200 - fmr) * eval / 200` | Hypersion has no FMR-eval damping. Affects pruning gates downstream. | LOW |
| T2 | Hypersion-own | Counter-move boost in MovePicker `score_quiets` | Hypersion's `counterMoves` table is only used for LMR reduction, NOT for move-scoring | LOW |
| T3 | Alexandria `threads.h:63` + `history.cpp:73` | Per-side root-only history `[2][64*64]`, used at ply==0 instead of mainHist | Hypersion uses flat mainHist at all plies | MEDIUM |
| T4 | Berserk `types.h:201` `caph[12][64][2][7]` | Capture history indexed by defender-status (defended/undefended) | Hypersion's captureHist has no defender-status dimension | MEDIUM |

## Execution order + gates

Sequential. Each: build → 30g triage → 200g confirm → optional LTC validation if borderline. Stop-loss: 2 of 4 reject = halt and ship.

1. **T1 FMR eval damping** first — smallest patch (4-line eval-site multiplier), lowest risk, fastest to validate.
2. **T2 Counter-move scoring** — depends on adding to MovePicker constructor, ~30 lines.
3. **T3 Root history** — new table per Worker, separate read/update paths at ply==0.
4. **T4 Defender-status capture history** — bumps captureHist dimensionality, structural change but contained.

## Verification

Each phase:
1. **Rule 2** — read SOURCE engine code at the cited line, confirm we understand the mechanism
2. **Rule 1** — cutechess SPRT via `testing/sprt.py`
3. **Rule 3** — search chessprogramming wiki if mechanism is unusual

## Final deliverable

**v3.2** with all SPRT-verified ships layered on v3.1. Tournament rerun for measured improvement vs v3.0/v3.1 baselines.
