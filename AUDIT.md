# Hypersion vs Stockfish 18 — Exhaustive Divergence Audit

Living document. Catalogs every observed divergence between
`C:\Engine\Hypersion\src\` and `C:\Engine\stockfish\src\`. Updated as
findings are applied. Tracks **305 total findings** across 4 audit passes.

## Categories
- **BUG**: clear semantic divergence; SF-source-backed fix exists
- **TUNING**: documented parameter difference (SPSA-tuned, tombstoned) — leave alone
- **DESIGN**: intentional architectural divergence (e.g. 5× eval scale)
- **STYLE**: cosmetic / formatting
- **LATENT**: code-level concern that doesn't currently bite
- **TODO**: deferred — too invasive for single fix, needs refactor

## Status
- `DONE <commit>` — fixed in named commit
- `TODO` — needs follow-up
- `DESIGN` / `TUNING` / `LATENT` / `STYLE` — won't fix (tracked for completeness)

---

## Applied this session (39 fixes across 11 commits)

| Commit | Fixes |
|---|---|
| `4c40726` | Syzygy narrow DTZ skip (KBNK/KBBK/KNNK use TB) |
| `643a89f` | 14 TB-threshold / is_decisive parity bugs |
| `092e4bc` | Root TB-WIN display (later revised in c7e06d3) |
| `3045981` | SEE_GE KING-case (CRITICAL), NNUE FC_1 padded width |
| `017239a` | TT cutoff `!excludedMove` (re-enables SE), improving fallback, ttMove depth-floor |
| `43838b8` | corrhist startup contamination cleared on disable |
| `43c9f15` | PersistCorrHist defaults OFF |
| `c7e06d3` | rm.score TB-WIN clobbering reverted (DTZ tiebreak restored) |
| `8f7dd3d` | Aspiration fail-low/high SF-style bounds |
| `96c90d1` | 12 main-search fixes (#21,#22,#24,#30,#35,#49,#51,#66,#108,#112,#129,#132) |
| `bd8309c` | 7 more main-search fixes (#12,#13,#17,#23,#57,#96,#131) |
| `20f5399` | 5 qsearch fixes (qs#5,#14/#29,#31,#32,#39) |
| `2c6b69e` | 6 misc fixes (main#8, qs#12, qs#20, uci#9, uci#19, uci#37) |

---

## search.cpp findings (140 entries) — outstanding TODO bugs

| # | Hypersion line | SF18 ref | Summary | Status |
|---|---|---|---|---|
| 4 | (missing) | search.cpp:630-635 | `upcoming_repetition` early-draw alpha upgrade missing | TODO — needs new `Position::upcoming_repetition` method |
| 8 | search.cpp:1900 | SF:670-671 | selDepth per-call update | DONE 2c6b69e |
| 12 | search.cpp:1895 | SF:709 | `ss->ttPv` stack write | DONE bd8309c |
| 13 | (no flag) | SF:710 | `ttCapture` computed | DONE bd8309c (currently maybe_unused) |
| 15 | search.cpp:1933 | SF:766-776 | TT-cut on quiet ttMove → history bonus missing | TODO — needs `update_quiet_histories` factor-out |
| 16 | search.cpp ~1893 | SF:780-799 | rule50≥96 TT-cutoff re-probe (graph-history workaround) | TODO — endgame correctness, complex |
| 17 | search.cpp:1953 | SF:802-810 | Syzygy probe `rule50==0 && !can_castle` gates | DONE bd8309c |
| 19 | search.cpp:1962-1968 | SF:828-852 | TB PvNode bestValue/maxValue clamp branch | TODO — TB-return refactor |
| 21 | search.cpp:1974 | SF:716-717 | in-check eval propagates `(ss-2)->staticEval` | DONE 96c90d1 |
| 22 | search.cpp:1973 | SF:718-719 | excludedMove reuses parent's static eval | DONE 96c90d1 |
| 23 | search.cpp:1975-1983 | SF:729-732 | TT-hit eval upgrade `eval = ttData.value` | DONE bd8309c |
| 24 | search.cpp:1979-1982 | SF:736-741 | First-visit eval cached as BOUND_NONE TT write | DONE 96c90d1 |
| 30 | search.cpp:2078 | SF:887-889 | RFP return `(2β+eval)/3` not raw staticEval | DONE 96c90d1 |
| 35 | search.cpp:2086 | SF:874 | Razor uses full window not null | DONE 96c90d1 |
| 49 | search.cpp:2225 | SF:973-975 | ProbCut TT write on cutoff | DONE 96c90d1 |
| 51 | search.cpp:2244 | SF:986-989 | Small-ProbCut bound bit-test | DONE 96c90d1 |
| 57 | search.cpp:2287 | SF:1390 | quietsTried/capturesTried array sizes | DONE bd8309c (bumped 64→128, 32→64) |
| 66 | search.cpp:2325 | SF:1103-1109 | Quiet futility `continue` not `skipQuiets` | DONE 96c90d1 |
| 85 | search.cpp:2456 | SF:1046,1191-1193 | ttPv LMR net sign — verify against SF aggregate | TODO (verify) — depends on understanding SF's r += 946 + later r -= bigger |
| 96 | search.cpp:2511 | SF:1216-1217 | Capture LMR statScore | DONE bd8309c |
| 108 | search.cpp:2554 | SF:1258-1259 | Post-LMR fail-high contHist bonus +1365 | DONE 96c90d1 |
| 112 | search.cpp:2565-2570 | SF:1380-1381 | Alpha-raise depth -= 2 for siblings | DONE 96c90d1 |
| 116 | search.cpp:2620 | SF:1835 | Separate malus formula (not `-bonus`) with moveCount taper | TODO — SPSA-sensitive, needs co-tune |
| 124 | (no path) | SF:1415-1421 | Alpha-raise-only nodes get post-loop history updates | TODO — needs `update_all_stats` factor-out |
| 125 | (no path) | SF:1424-1444 | Fail-low bonus to prior opponent quiet | TODO |
| 126 | (no path) | SF:1448-1453 | Fail-low bonus for prior capture | TODO |
| 129 | (no path) | SF:1407-1408 | Fail-high bestValue moderation toward beta | DONE 96c90d1 |
| 130 | (no maxValue) | SF:1455-1456 | PvNode bestValue clamp by TB maxValue | TODO — depends on #19 |
| 131 | search.cpp:2670 | SF:1460-1461 | Fail-low ttPv bestow from parent | DONE bd8309c |
| 132 | search.cpp:2675 | SF:1465 | TT write missing !excludedMove guard | DONE 96c90d1 |
| 136 | search.cpp:2684 | SF:1475-1478 | Corrhist update bound-direction `(bestValue > staticEval) == bool(bestMove)` | TODO — SPSA-sensitive |

### Remaining 110 main-search entries (TUNING / DESIGN / STYLE / LATENT — won't fix)

These are documented in the full audit dump from the agent but not transcribed here individually (they cover SF tuning constants, intentional architectural divergences, cosmetic differences, and latent code-level concerns that don't currently bite). Examples: per-iteration `failedHighCnt` depth reduction (LATENT/TODO), missing `correctionValue` aggregate (DESIGN), missing `averageScore` root field (TODO), missing `nmpMinPly` plumbing (DESIGN), various tuning constants. All catalogued in session transcripts.

---

## qsearch findings (40 entries) — outstanding TODO

| # | Summary | Status |
|---|---|---|
| qs #5 | MAX_PLY-in-check returns VALUE_DRAW not raw eval | DONE 20f5399 |
| qs #12 | Apply corrhist to qsearch stand-pat eval | DONE 2c6b69e |
| qs #14/#29 | Fail-high bestValue moderation `(bestValue+beta)/2` | DONE 20f5399 |
| qs #18 | Pass contHist to qsearch MovePicker for evasion ordering | TODO — needs MovePicker ctor extension |
| qs #19 | Stalemate-detect when no-captures + piece-down + no pawn-push | TODO — needs movecount tracking + special check |
| qs #20 | Recapture exemption from capture-futility (`m.to_sq == prevSq`) | DONE 2c6b69e |
| qs #21 | SEE prune threshold 0 → -80 cp (matches SF) | TODO — SPSA-sensitive |
| qs #23 | Per-victim capture-futility (futilityBase + PieceValue[victim]) | TODO — refactor capture-futility logic |
| qs #31 | Never write BOUND_EXACT from qsearch | DONE 20f5399 |
| qs #32 | Preserve sticky ttPv on qsearch TT save | DONE 20f5399 |
| qs #39 | Write ss->staticEval BEFORE stand-pat short-circuit | DONE 20f5399 |

### Remaining ~29 qsearch entries (DESIGN / STYLE / LATENT)

Documented in audit transcripts. Examples: different node-accounting convention (DESIGN), tombstoned ttValue stand-pat upgrade (DESIGN), `is_valid(ttData.value)` race-guard (LATENT), `ss->continuationHistory` Stack field architecture difference (DESIGN), root PV array update missing (LATENT, cosmetic for analysis users).

---

## uci.cpp / main.cpp / book.cpp / perft.cpp findings (125 entries) — outstanding TODO

| # | File | Summary | Status |
|---|---|---|---|
| uci [2] | uci.cpp:27-29 | Fixed `states[MAX_GAME_PLIES]` overflow risk | LATENT — needs dynamic state list |
| uci [9] | uci.cpp:470 | UCI_Elo clamp [500,3300] (was [500,3200]) | DONE 2c6b69e |
| uci [19] | uci.cpp:416 | `Clear Hash` also clears thread histories | DONE 2c6b69e |
| uci [22] | uci.cpp:437 | `EvalUseSmallOnly` has no backing Options field | LATENT — option-state inconsistency |
| uci [25] | uci.cpp:457 | `UCI_Chess960` declared but castling stays orthodox | LATENT — false advertising |
| uci [29] | uci.cpp:245 | `isready` returns immediately without waiting | TODO — needs `wait_for_search_finished` semantics |
| uci [34] | uci.cpp:285 | Static `states[]` reused not reallocated | LATENT — same root as [2] |
| uci [37] | uci.cpp:138 | parse_uci_move case-sensitive | DONE 2c6b69e |
| uci [45] | uci.cpp:298 | `cmd_go` doesn't capture startTime — book + setup latency uncounted | TODO — small but real, time-management drift |
| uci [48] | uci.cpp:369 | No `verify_networks()` step before `go` — bad EvalFile crashes at search-time | LATENT — bad-config UX |
| uci [99] | book.cpp:174 | Dead opening-variety file load on every startup | LATENT — perf waste, dead feature |
| uci [100] | book.cpp:179 | Same as above (`recent_first_moves` I/O) | LATENT |
| perft [116] | perft.cpp:36 | Move formatter chess960 king-takes-rook | LATENT — Chess960 not supported |
| main, book, perft various | various | 115+ DESIGN/STYLE differences | not fixing (intentional Hypersion arch) |

---

## Other-file audits done earlier this session

- `position.cpp` — SEE_GE KING-case (fixed 3045981) + 6 LATENT/STYLE findings
- `movepick.cpp` — score_evasions contHist (fixed 643a89f) + 4 LATENT
- `nnue.cpp` — knight value (fixed) + FC_1 padded (fixed 3045981) + 2 LATENT
- `tt.cpp` — value_to_tt/from_tt ply range (fixed 643a89f) + minor STYLE
- `timeman.cpp` — no major divergences (audit done)
- `evaluate.cpp` — no major bugs (audit done, NNUE-only path dominates)
- `bitboard.cpp` — no findings
- `history.h` — no major divergences (intentional 2-deep contHist, asymmetric history bonus tombstoned)

---

## Session verification

| Metric | Pre-session | Final |
|---|---|---|
| Bench Threads=1 | 4.85M nodes | **2.51M nodes** (48% fewer) |
| KBBK Syzygy ON (22g) | 22/22 mate | **22/22 mate** |
| KBBK Syzygy OFF (22g) | 13W/9D (59%) | **16-17W/5-6D (73-77%)** |
| Match vs HybridV17 (30g) | (not run) | **26W-0L-4D, +458 Elo, 100% LOS** |

Working tree clean; final binary at `C:\Engine\Hypersion\Hypersion.exe` includes all 39 fixes.

---

## What remains for a future session

**HIGH-priority TODOs (clear bugs, refactor needed)**:
1. `Position::upcoming_repetition` method + call site (#4)
2. `update_quiet_histories` helper factor-out → unlocks #15, #124, #125, #126
3. TB PvNode `maxValue` clamp refactor (#19, #130)
4. `isready` blocking semantics (uci #29)
5. `cmd_go` startTime capture (uci #45)

**MEDIUM-priority** (SPSA-sensitive, need re-tune):
6. Separate malus formula with moveCount taper (#116)
7. Corrhist bound-direction check (#136)
8. ttPv LMR sign verification (#85)
9. qsearch SEE threshold 0 → -80 (qs #21)

**qsearch refactors**:
10. Pass contHist to qsearch MovePicker (qs #18)
11. Stalemate-detect fallback (qs #19)
12. Per-victim capture-futility (qs #23)

**LATENT cleanups** (no current effect but worth fixing eventually):
13. Replace `states[MAX_GAME_PLIES]` with dynamic list (uci [2], [34])
14. Either implement Chess960 properly or remove the option (uci [25])
15. Verify networks at startup with clear error message (uci [48])
16. Remove dead opening-variety code from book.cpp (uci [99]-[100])
17. EvalUseSmallOnly backing field (uci [22])
