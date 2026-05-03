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
10. **Book variety** — lower opening-book filter threshold (`bestW/4` →
    `bestW/12`) and switch to uniform weighting (was sqrt-weighted) among
    surviving moves. Same opening family, but real shuffle within playable
    lines — playing the same opponent twice gives different games. Pure
    user-facing behaviour change, no claimed strength impact.
11. **Stockfish-18 LMR history correction** — in the Late Move Reductions
    block, after the static adjustments (improving / cutNode / PV / etc.),
    reduce `r` further by `statScore / 8192`, where
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
