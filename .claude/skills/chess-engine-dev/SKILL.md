---
name: chess-engine-dev
description: Deep chess-programming knowledge for Hypersion development — SF18 NNUE architecture, search heuristics, ELO testing theory, and common engine bugs. Invoke when discussing engine internals, planning experiments, or diagnosing eval/search behavior.
---

# Chess engine development reference

This skill loads condensed, high-signal knowledge for working on Hypersion.
It is NOT a tutorial — it is a quick-reference for an engineer who already
knows the basics. For the project's own conventions (SPRT protocol, build
flags, file layout) read `CLAUDE.md` at the project root.

## When to invoke this skill

- Designing a new heuristic ("should we add improving-moves?")
- Diagnosing eval drift, NPS regressions, search instability
- Porting a feature from Stockfish — what to compare, what's already tried
- Sanity-checking a proposed test design

## How to use

The reference files in `references/` cover one topic each. Read the
filename listing below and `Read` the specific file you need — don't load
all at once. Files use < 200 lines each so you can grep them quickly.

### Reference index

| File | When to read |
|---|---|
| `references/sf18-nnue-arch.md`     | Working on NNUE forward, accumulators, FT, net selection |
| `references/search-heuristics.md`  | Tweaking LMR, NMP, SE, pruning constants |
| `references/move-ordering.md`      | MovePicker stages, history tables, killer/counter moves |
| `references/elo-testing-theory.md` | Designing SPRT, choosing sample size, interpreting CI |
| `references/common-bugs.md`        | Eval drift, NPS regressions, threading non-determinism |
| `references/sf-source-map.md`      | Where each concept lives in `C:\Engine\stockfish\src\` |
| `references/nps-optimization.md`   | Profiling, SIMD pitfalls, cache effects |

### Workflow when starting a new optimization

1. **Decide the hypothesis** — what specific search/eval change might help?
2. **Check `references/common-bugs.md`** — has this class of change been
   tried? Look for tombstones in `src/` mentioning your area.
3. **Read the relevant SF18 file** (`references/sf-source-map.md` lists paths).
4. **Plan the SPRT** — concurrency=6 for search-logic, concurrency=2 for
   memory-aggressive (per `CLAUDE.md`).
5. **Run Stage 1 triage** (30g) before Stage 2 (200g).
6. **Tombstone if neutral/negative** with ELO ± CI in the source.

## Critical anti-patterns

- **Single-run bench comparisons** — Hypersion bench is non-deterministic
  across processes. NPS comparisons need 5+ runs averaged.
- **30g positives in [+5, +50]** — almost always regress at 200g. Don't
  ship without confirmation. See PROTOCOL.md "30g Fakeout Pattern".
- **Trusting the eval bar** — Hypersion's internal eval is at SF's 5×
  scale. UCI `score cp` divides by 5; `eval` command divides by 5; the
  search internals don't.
- **Porting SF18 features one-at-a-time without re-tuning surrounding
  margins** — many SF features depend on coordinated tuning. Single-feature
  ports often regress (see Phase 2/3/6 sweep tombstones).

## Critical things that DID help (history)

| Change | Result |
|---|---|
| L1-transform SIMD vectorization | +23.2 ± 64.9 ELO @ conc=2 (commit 3e69bcb) |
| 2-ply continuation history (half weight) | +34.9 ELO @ 200g |
| Threat-by-lesser move-ordering bonus | (in shipped code, see movepick.cpp::score_quiets) |
| Material/endgame time scaling | (in shipped code, see search.cpp:734-738) |

For neutrals/negatives, see source tombstones (grep for `// NOTE: tested`).
