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
10. **Lynx-style score-stability time factor** —
    `scale *= 2^(clamp(prevScore - bestScore, -100, 100) / 100)` at depth ≥ 7.
    Range [0.5, 2.0]: score dropped 100 cp → ×2.0, score rose 100 cp → ×0.5.
    Constants ported verbatim from Lynx's fishtest-tuned implementation.
    A/B vs v1.0: **+26 ELO** (13W/17D/10L over 40 games at TC 10+0.1).
11. **Lynx-style 5-bucket bestmove-stability** —
    `LYNX_BM_STAB[5] = { 2.50, 1.20, 0.90, 0.80, 0.75 }` indexed by
    consecutive-same-move iterations. Replaces the prior 2-bucket
    `{≥2 → 0.75, ≥4 → 0.5}` scheme plus the `+1.4×` volatility bonus.
    Isolated A/B on top of score-stability: **+80 ELO** (13W/23D/4L).
12. **Lynx-style separate bonus/malus formulas** for history updates —
    `bonus(d) = min(2440, 243 + 178·d + 3·d²)`, `malus(d) = min(1473, 220 + 253·d + 7·d²)`.
    Failed-sibling demotion now uses `-malus` instead of `-bonus`.
    Lynx tunes them independently (malus has steeper d², lower cap).

Cumulative effect on `main` vs Hypersion 1.0:
**+158 ELO** (19W/19D/2L over 40 games at TC 10+0.1).
Only 2 losses out of 40 — clear release-worthy improvement.

Tested-but-reverted (logged so they don't get retried blind):
- Worsening flag in LMR (eyeballed magnitudes)
- Score-drop time extension (eyeballed magnitudes — Lynx port superseded)
- History decay on `ucinewgame` (halve instead of zero)
- `Move Overhead` default 30 → 100 ms (ate the 10+0.1 increment, −26 ELO)
- `UCI_Variant` combo (broke python-chess / lichess-bot)

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
