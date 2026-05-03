# Hypersion CHANGELOG

## Hypersion 2 (2026-05-03)

### Eval

- `Position::is_draw()` now detects insufficient-material draws:
  - K vs K
  - K + (one minor) vs K
  - K + B vs K + B with same-colour bishops
  Returns true *before* NNUE is called, saving inference cost and
  preventing the engine from trading down into known draws.

### UCI / protocol

- `go searchmoves m1 m2 ...` now restricts the root search to the
  specified moves. Required by some tournament managers and
  OpenBench-style tooling.

### Lichess bot

- Removed the in-tree `lichess_bot/` mini-bot from the public repo and
  switched local runs to the official `lichess-bot-devs/lichess-bot`
  framework with `config-hypersion.yml`.
- Pondering enabled in the framework config (`ponder: true`) — the bot
  thinks on the opponent's clock. Hypersion's engine already handled
  `go ponder` / `ponderhit` correctly, the framework just needed to
  send those commands.

### Investigated and reverted

The following changes were tested but regressed −35 ELO when bundled,
so they were reverted before shipping. Likely root cause: each one
needs careful per-parameter tuning that wasn't done in this pass.

- **Worsening flag in LMR** — `staticEval < (ss-2)->staticEval - 25`
  loosening reductions in deteriorating positions. Reverted.
- **Score-drop time extension** — extending optimum time by 1.15× /
  1.3× / 1.6× when this iteration's score dropped 40 / 80 / 150 cp.
  Reverted.
- **History decay on `ucinewgame`** — halving instead of zeroing.
  Reverted; full clear retained.

### Strength

A/B vs Hypersion 1.0 at TC 10+0.1 (40 games):
**v2 wins +17 ELO** (9W / 24D / 7L, 52.5%).

### Build / release

- Released as `v2.0` on 2026-05-03 with `Hypersion-2-windows-x64.zip`
  (Hypersion.exe + both NNUE files + LICENSE + README.txt).

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
