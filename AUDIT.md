# Hypersion vs Stockfish 18 — Exhaustive Divergence Audit (2026-05-17)

**140 differences catalogued in `search.cpp::search()` alone.** Other files have separate audits done earlier this session.

## Categories
- **BUG**: clear semantic divergence; SF-source-backed fix exists
- **TUNING**: documented parameter difference (SPSA-tuned, tombstoned) — leave alone
- **DESIGN**: intentional architectural divergence
- **STYLE**: cosmetic / formatting
- **LATENT**: code-level concern that doesn't currently bite
- **TODO**: deferred — too invasive for single fix

---

## Actionable BUG findings (SF reference, not tombstoned)

| # | Hypersion line | SF18 ref | One-line summary | Status |
|---|---|---|---|---|
| 4 | (missing) | search.cpp:630-635 | `upcoming_repetition` early-draw alpha upgrade missing | TODO |
| 8 | search.cpp ~1500 (print_info) | SF:670-671 | `selDepth = max(selDepth, ss->ply+1)` per-call update missing | TODO |
| 12 | search.cpp:1895 | SF:709 | `ss->ttPv` declared in Stack but never read/written across recursion | TODO |
| 13 | (no ttCapture flag) | SF:710 | `ttCapture` never computed; multiple downstream SF heuristics blocked | TODO |
| 15 | search.cpp:1933 (TT cutoff) | SF:766-776 | When TT-cut on quiet ttMove with ttValue>=beta, no history bonus awarded | TODO |
| 16 | search.cpp TT cutoff | SF:780-799 | High-rule50 (>=96) TT cutoff re-probe missing — endgame correctness | TODO |
| 17 | search.cpp:1953 | SF:802-810 | Syzygy probe missing castling-rights / rule50==0 gates | TODO |
| 19 | search.cpp:1962-1968 | SF:828-852 | TB PvNode bestValue/maxValue clamp branch missing | TODO |
| 21 | search.cpp:1974 (in-check) | SF:716-717 | In-check sets `ss->staticEval=VALUE_NONE`; SF propagates `(ss-2)->staticEval` | **APPLY** |
| 22 | search.cpp:1973-1974 | SF:718-719 | No `excludedMove` branch — re-evaluates NNUE inside SE recursion | **APPLY** |
| 23 | search.cpp:1975-1983 | SF:729-732 | TT-hit eval upgrade `eval = ttData.value` when bound-consistent missing | TODO |
| 24 | search.cpp:1979-1982 | SF:736-741 | First-visit eval doesn't get cached with `BOUND_NONE` TT write | **APPLY** |
| 30 | search.cpp:2078 | SF:887-889 | RFP returns `staticEval`; SF returns `(2*beta + eval)/3` (weighted moderation) | **APPLY** |
| 35 | search.cpp:2086-2087 | SF:874 | Razor recurses qsearch null-window; SF uses full window | **APPLY** |
| 49 | search.cpp:~2225 | SF:973-975 | ProbCut TT write on cutoff missing — re-search savings lost | **APPLY** |
| 51 | search.cpp:2244 | SF:986-989 | `tte->bound() == BOUND_LOWER` exact-equality misses BOUND_EXACT (which has LOWER bit) | **APPLY** |
| 57 | search.cpp:2287-2295 | SF:1390 | `quietsTried[64]/capturesTried[32]` silently drop overflow | LATENT |
| 66 | search.cpp:2325 | SF:1103-1109 | Quiet futility sets `skipQuiets=true` (cancels all); SF `continue`s per-move | **APPLY** |
| 85 | search.cpp:2456 | SF:1046-1047,1191-1193 | ttPv LMR sign: SF `r += 946` ttPv block but `r -= ...` later — verify net | TODO (verify) |
| 96 | search.cpp:2511 (`if !isCapture`) | SF:1216-1217 | Captures get no LMR statScore | TODO |
| 108 | search.cpp:~2554 | SF:1258-1259 | Post-LMR fail-high `update_continuation_histories(... +1365)` missing | **APPLY** |
| 112 | search.cpp:2565-2570 | SF:1380-1381 | After alpha-raise at depth [3,13], `depth -= 2` for siblings — missing | **APPLY** |
| 116 | search.cpp:2620 | SF:1835 | malus = symmetric `-bonus`; SF has separate formula with moveCount taper | TODO |
| 124 | (no path) | SF:1415-1421 | Alpha-raised-but-not-beta-cut nodes get NO history updates | TODO |
| 125 | (no path) | SF:1424-1444 | Fail-low bonus to prior opponent move missing | TODO |
| 129 | (no path) | SF:1407-1408 | `bestValue = (bestValue*depth + beta)/(depth+1)` fail-high moderation missing | **APPLY** |
| 130 | (no maxValue) | SF:1455-1456 | `bestValue = min(bestValue, maxValue)` PV TB clamp missing | TODO |
| 131 | search.cpp:~2670 | SF:1460-1461 | `ss->ttPv |= (ss-1)->ttPv` fail-low ttPv bestow from parent | TODO (depends on #12) |
| 132 | search.cpp:2675-2678 | SF:1465 | TT write missing `!excludedMove` guard — SE inner corrupts outer TT entry | **APPLY** |
| 136 | search.cpp:2684 | SF:1475-1478 | Corrhist update condition `bestMove != none && !pos.capture` vs SF's bound-direction `(bestValue > staticEval) == bool(bestMove)` | TODO |

## Already-applied this session (recap)
- `(next)` — #12, #13, #17, #23, #57, #96, #131 (7 fixes)
- `96c90d1` — #21, #22, #24, #30, #35, #49, #51, #66, #108, #112, #129, #132 (12 fixes)
- `c7e06d3` — rm.score TB-WIN clobbering reverted (DTZ tiebreak restored)
- `8f7dd3d` — Aspiration fail-low/high SF-style bounds
- `43c9f15` — PersistCorrHist defaults OFF
- `43838b8` — corrhist startup contamination cleared on disable
- `017239a` — TT cutoff !excludedMove, improving fallback, ttMove depth-floor
- `3045981` — SEE_GE KING-case, NNUE FC_1 padded
- `092e4bc` — root TB-WIN display (revised in c7e06d3)
- `643a89f` — 14 TB-threshold/is_decisive parity bugs
- `4c40726` — Syzygy narrow DTZ skip (KBNK/KBBK/KNNK)

## Plan for this session

Apply 8 **APPLY**-marked findings in single batch:
- #21, #22, #24 (eval pipeline)
- #30, #129 (fail-high moderation)
- #35 (razor full window)
- #49 (ProbCut TT write)
- #51 (small-ProbCut bound bit-test)
- #66 (quiet futility continue)
- #108 (post-LMR contHist)
- #112 (alpha-raise depth reduction)
- #132 (SE TT-write guard)

Verify with bench + KBBK regression test; commit; document remaining TODOs.

---

## Other-file audits done earlier this session
- `position.cpp` — SEE_GE KING-case bug (fixed 3045981); 6 other low-impact findings
- `movepick.cpp` — score_evasions contHist (fixed 643a89f); 4 LATENT
- `nnue.cpp` — material knight 776→781 (fixed); FC_1 padded width (fixed 3045981)
- `tt.cpp` — value_to_tt/value_from_tt ply range (fixed 643a89f)
- `timeman.cpp` — no major divergences (audit done)
- `evaluate.cpp` — no major bugs found (audit done)
- `bitboard.cpp` — no findings
