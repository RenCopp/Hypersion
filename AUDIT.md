# Hypersion vs Stockfish 18 — Exhaustive Divergence Audit

Living document. Catalogs every observed divergence between
`C:\Engine\Hypersion\src\` and `C:\Engine\stockfish\src\`. Updated as
findings are applied. Tracks **335 total findings** across 5 audit passes
(5th pass: misc.cpp, movegen.cpp, syzygy.cpp, zobrist.cpp — added 2026-05-17).

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

## Applied this session (57 fixes across 16 commits)

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
| `037745f` | 3 LATENT cleanups (uci#22 backing field, uci#48 nnue-load warning, uci#99/#100 dead book code) |
| `58a717c` | 2 audit fixes from 5th-pass (movegen CAPTURES underpromotion, zobrist noPawns seed) |
| `85e9ac4` | Tier 1+2: TB maxValue+upcoming_repetition+cmd_go startTime+zobrist promo-rank zero |
| `faec2e0` | Tier 1+5: update_quiet_history helper + TT-cut quiet ttMove bonus + LEGAL filter opt |
| `3cec85e` | Tier 3+4 batch (**SHIPPED +79.5±42.8 ELO @ 200g 5+0.05 conc=6, 98W-53L-49D**): qs#21 SEE-80, qs#18 contHist, qs#23 per-victim, #85 verify, #116 malus taper, #125 prior-quiet, #136 corrhist bound-dir |

---

## search.cpp findings (140 entries) — outstanding TODO bugs

| # | Hypersion line | SF18 ref | Summary | Status |
|---|---|---|---|---|
| 4 | (missing) | search.cpp:630-635 | `upcoming_repetition` early-draw alpha upgrade missing | DONE 85e9ac4 (cuckoo table + 2 call sites) |
| 8 | search.cpp:1900 | SF:670-671 | selDepth per-call update | DONE 2c6b69e |
| 12 | search.cpp:1895 | SF:709 | `ss->ttPv` stack write | DONE bd8309c |
| 13 | (no flag) | SF:710 | `ttCapture` computed | DONE bd8309c (currently maybe_unused) |
| 15 | search.cpp:1933 | SF:766-776 | TT-cut on quiet ttMove → history bonus missing | DONE faec2e0 (TT-cut quiet ttMove bonus) |
| 16 | search.cpp ~1893 | SF:780-799 | rule50≥96 TT-cutoff re-probe (graph-history workaround) | TODO — endgame correctness, complex |
| 17 | search.cpp:1953 | SF:802-810 | Syzygy probe `rule50==0 && !can_castle` gates | DONE bd8309c |
| 19 | search.cpp:1962-1968 | SF:828-852 | TB PvNode bestValue/maxValue clamp branch | DONE 85e9ac4 (maxValue + tbAlphaFloor) |
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
| 85 | search.cpp:2456 | SF:1046,1191-1193 | ttPv LMR net sign — verify against SF aggregate | VERIFIED 3cec85e (Hypersion --r matches SF net negative; smaller magnitude at integer-ply granularity) |
| 96 | search.cpp:2511 | SF:1216-1217 | Capture LMR statScore | DONE bd8309c |
| 108 | search.cpp:2554 | SF:1258-1259 | Post-LMR fail-high contHist bonus +1365 | DONE 96c90d1 |
| 112 | search.cpp:2565-2570 | SF:1380-1381 | Alpha-raise depth -= 2 for siblings | DONE 96c90d1 |
| 116 | search.cpp:2620 | SF:1835 | Separate malus formula (not `-bonus`) with moveCount taper | DONE 3cec85e (taper applied; full separate magnitude needs SPSA) |
| 124 | (no path) | SF:1415-1421 | Alpha-raise-only nodes get post-loop history updates | TODO — needs `update_all_stats` factor-out |
| 125 | (no path) | SF:1424-1444 | Fail-low bonus to prior opponent quiet | DONE 3cec85e |
| 126 | (no path) | SF:1448-1453 | Fail-low bonus for prior capture | TODO — needs piece-captured-by-prior tracking |
| 129 | (no path) | SF:1407-1408 | Fail-high bestValue moderation toward beta | DONE 96c90d1 |
| 130 | (no maxValue) | SF:1455-1456 | PvNode bestValue clamp by TB maxValue | DONE 85e9ac4 (along with #19) |
| 131 | search.cpp:2670 | SF:1460-1461 | Fail-low ttPv bestow from parent | DONE bd8309c |
| 132 | search.cpp:2675 | SF:1465 | TT write missing !excludedMove guard | DONE 96c90d1 |
| 136 | search.cpp:2684 | SF:1475-1478 | Corrhist update bound-direction `(bestValue > staticEval) == bool(bestMove)` | DONE 3cec85e |

### Remaining 110 main-search entries (TUNING / DESIGN / STYLE / LATENT — won't fix)

These are documented in the full audit dump from the agent but not transcribed here individually (they cover SF tuning constants, intentional architectural divergences, cosmetic differences, and latent code-level concerns that don't currently bite). Examples: per-iteration `failedHighCnt` depth reduction (LATENT/TODO), missing `correctionValue` aggregate (DESIGN), missing `averageScore` root field (TODO), missing `nmpMinPly` plumbing (DESIGN), various tuning constants. All catalogued in session transcripts.

---

## qsearch findings (40 entries) — outstanding TODO

| # | Summary | Status |
|---|---|---|
| qs #5 | MAX_PLY-in-check returns VALUE_DRAW not raw eval | DONE 20f5399 |
| qs #12 | Apply corrhist to qsearch stand-pat eval | DONE 2c6b69e |
| qs #14/#29 | Fail-high bestValue moderation `(bestValue+beta)/2` | DONE 20f5399 |
| qs #18 | Pass contHist to qsearch MovePicker for evasion ordering | DONE 3cec85e |
| qs #19 | Stalemate-detect when no-captures + piece-down + no pawn-push | TODO — needs movecount tracking + special check; very rare case |
| qs #20 | Recapture exemption from capture-futility (`m.to_sq == prevSq`) | DONE 2c6b69e |
| qs #21 | SEE prune threshold 0 → -80 cp (matches SF) | DONE 3cec85e (Value(-400) at 5x scale) |
| qs #23 | Per-victim capture-futility (futilityBase + PieceValue[victim]) | DONE 3cec85e |
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
| uci [22] | uci.cpp:437 | `EvalUseSmallOnly` has no backing Options field | DONE 037745f |
| uci [25] | uci.cpp:457 | `UCI_Chess960` declared but castling stays orthodox | LATENT — kept for GUI compat (lichess-bot expects the option) |
| uci [29] | uci.cpp:245 | `isready` returns immediately without waiting | NOT-A-BUG — SF18 also prints readyok immediately (uci.cpp:137) |
| uci [34] | uci.cpp:285 | Static `states[]` reused not reallocated | LATENT — same root as [2] |
| uci [37] | uci.cpp:138 | parse_uci_move case-sensitive | DONE 2c6b69e |
| uci [45] | uci.cpp:298 | `cmd_go` doesn't capture startTime — book + setup latency uncounted | DONE 85e9ac4 (lim.goStartTime + TimeManager init uses it) |
| uci [48] | uci.cpp:369 | No `verify_networks()` step before `go` — silent classical-fallback on bad EvalFile | DONE 037745f (now emits warning) |
| uci [99] | book.cpp:174 | Dead opening-variety file load on every startup | DONE 037745f (removed entirely) |
| uci [100] | book.cpp:179 | Same as above (`recent_first_moves` I/O) | DONE 037745f |
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

## 5th audit pass (2026-05-17) — misc / movegen / syzygy / zobrist

### movegen.cpp (10 findings)

| # | Hypersion location | SF18 ref | Summary | Status |
|---|---|---|---|---|
| mg #1 | movegen.cpp:20-29 | movegen.cpp:108-124 | make_promotions signature: SF18 uses `<Type, Direction D, bool Enemy>` template, splits Q vs underpromotions by capture-vs-push | DONE 58a717c (added `Enemy` param) |
| mg #2 | movegen.cpp:21 | movegen.cpp:113 | CAPTURES mode emitted only Q for all promotions (incl. capture-promotions). Lost capture-underpromotions in qsearch (knight-capture-promo for smothered-mate). | DONE 58a717c |
| mg #3 | movegen.cpp:80-91 | movegen.cpp:188-202 | EP-in-EVASIONS check uses different formulation (`target & (ep-Up)` vs SF's `target & (ep+Up) return early`). Verified equivalent in all reachable positions — both correctly handle direct-pawn-check and discovered-check cases. | LATENT — semantically equivalent, just different code path |
| mg #4 | movegen.cpp:215-226 | movegen.cpp:293-310 | LEGAL filter calls `pos.legal()` on every move; SF18 only checks for pinned/king/EN_PASSANT moves. | DONE faec2e0 (pinned-only filter) |
| mg #5 | movegen.cpp:114-156 | movegen.cpp:247-261 | Castling: Hypersion does inline Chess960 validation (min/max + per-square attacker check); SF18 uses pos.castling_impeded() helper. | STYLE — both correct |
| mg #6 | movegen.h:39 | movegen.h:39-47 | ExtMove: Hypersion uses composition (`Move + int`), SF18 uses inheritance | DESIGN |
| mg #7 | movegen.cpp:173-175 | movegen.cpp:239-242 | EVASIONS target: Hypersion uses BetweenBB lookup; SF18 calls between_bb() helper | STYLE |
| mg #8 | movegen.cpp (absent) | movegen.cpp:37-87 | SF18 has AVX-512 splat_pawn_moves SIMD fast-path; Hypersion has no AVX-512 path | DESIGN — Hypersion targets AVX2 per CLAUDE.md |
| mg #9 | (constexpr branches) | (unified loop) | EP-evasion: Hypersion repeats `pawnsNotOn7 & pawn_attacks_bb(...)` twice in separate constexpr branches | LATENT — redundant compute, no perf impact |
| mg #10 | movegen.cpp:36-37 | movegen.cpp:131-133 | TRank/Up naming: Hypersion inlines direction; SF18 calls pawn_push(Us) helper | STYLE |

### zobrist.cpp (9 findings)

| # | Hypersion location | SF18 ref | Summary | Status |
|---|---|---|---|---|
| zb #1 | zobrist.cpp:11, 23 | position.cpp:51, 136, 346 | `Zobrist::noPawns` initialized but pawnKey starts at 0 (not noPawns). SF18 seeds `st->pawnKey = Zobrist::noPawns` so no-pawn positions get a unique nonzero key. | DONE 58a717c |
| zb #2 | position.cpp:230 | position.cpp:126-127 | Hypersion doesn't zero-out promotion-rank slots in psq[PAWN][SQ_*8/*1]; SF18 zeroes them. No practical impact (pawns can't legally exist on promotion ranks). | DONE 85e9ac4 |
| zb #3 | position.cpp:243-246 | (incremental) | materialKey computed eagerly via piece-count indexing in set_state; SF18 manages it incrementally only. Same slot semantics. | DESIGN — both correct |
| zb #4 | zobrist.h | position.h | minorKey tracking (KNIGHT/BISHOP/KING) — SF18 has no minorKey (no corrhist). | DESIGN — Hypersion-specific feature |
| zb #5 | zobrist.cpp:14 | position.cpp:120 | PRNG seed identical (1070372ULL) | STYLE |
| zb #6 | zobrist.cpp:7-10 | position.cpp:48-51 | Same psq/enpassant/castling array shapes | STYLE — identical |
| zb #7 | zobrist.cpp:18-19 | position.cpp:129-130 | EP key gen identical | STYLE |
| zb #8 | zobrist.cpp:20-21 | position.cpp:132-133 | Castling key gen identical | STYLE |
| zb #9 | zobrist.h vs inline | (Position::init) | Hypersion: separate `Zobrist::init()`; SF18: inline in Position::init() | STYLE |

### syzygy.cpp (12 findings) — all DESIGN (Fathom vs SF18 native TB probe)

All Hypersion-vs-SF18 differences in syzygy.cpp / syzygy.h are intentional
DESIGN divergences from the Fathom backend choice. The only "BUG" candidate
(legacy `probe_root` at line 88 with `rule50 != 0` gate) is documented dead
code — search.cpp:1041 uses `probe_root_dtz` exclusively, which correctly
handles rule50 inside. Fathom hang workaround (lines 147-170) is necessary
and correct (Fathom hangs on KQK/KRK with specific king alignments).

### misc.cpp (19 findings) — all DESIGN (intentional minimalism)

Hypersion's misc.cpp is intentionally minimal (19 lines vs SF18's 528 lines).
The agent flagged many "linker errors" from missing functions (dbg_*, Logger,
compiler_info, prefetch, MultiArray, etc.) but Hypersion doesn't call any of
them — it has its own per-class prefetch methods (TT::prefetch, CorrHist::
prefetch). No real bugs. All DESIGN divergences from intentional pruning.

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
14. Chess960 (uci [25]) — kept as silent-absorb option for lichess-bot GUI compat; would need full Chess960 implementation to advertise honestly
15. ~~Verify networks at startup with clear error message (uci [48])~~ — DONE 037745f
16. ~~Remove dead opening-variety code from book.cpp (uci [99]-[100])~~ — DONE 037745f
17. ~~EvalUseSmallOnly backing field (uci [22])~~ — DONE 037745f
18. LEGAL filter perf: skip pos.legal() for non-pinned, non-king, non-EP moves (mg #4) — small refactor, perft-affecting only
19. zobrist promotion-rank zero-out (zb #2) — no practical impact, low priority

---

## Plan for refactor-needed items (deferred work)

### TIER 1 — clear bugs, plain refactor — DONE

**1. `update_quiet_histories` helper factor-out** — DONE faec2e0
   (helper extracted; #15 TT-quiet bonus added; #124/#125/#126 deferred —
    those need SPSA-tuned bonus magnitudes co-calibrated with SF's history
    scales, which Hypersion's 0..2000 cap doesn't share)

**2. `Position::upcoming_repetition` + early-draw alpha upgrade** — DONE 85e9ac4

**3. TB PvNode `maxValue` clamp** — DONE 85e9ac4

### TIER 2 — UCI / time-management — DONE

**4. `isready` blocking semantics** — NOT-A-BUG (SF18 also prints readyok immediately)

**5. `cmd_go` startTime capture** — DONE 85e9ac4

### TIER 3 — qsearch tweaks — SHIPPED 3cec85e (UNVERIFIED, needs SPRT)

**6. qsearch SEE threshold** — DONE 3cec85e
**7. qsearch contHist in MovePicker** — DONE 3cec85e
**8. qsearch stalemate-detect** (qs #19) — STILL TODO (rare case,
     would need a movecount-tracked legal-move check inside qsearch)
**9. qsearch per-victim capture-futility** — DONE 3cec85e

### TIER 4 — SPSA-sensitive — SHIPPED 3cec85e (UNVERIFIED)

**10. Malus moveCount taper** (#116) — DONE 3cec85e (taper applied;
     full separate-magnitude-cap split still SPSA-deferred)
**11. Corrhist bound-direction** (#136) — DONE 3cec85e
**12. ttPv LMR sign** (#85) — VERIFIED ALREADY CORRECT, no change
**13. Fail-low prior-quiet bonus** (#125) — DONE 3cec85e
**13b. Alpha-raise-no-cutoff history** (#124) — STILL TODO (requires
      moving cutoff-internal history updates to post-loop)
**13c. Fail-low prior-capture bonus** (#126) — STILL TODO (requires
      tracking the piece captured by the prior move)

### TIER 5 — small remaining LATENT (no urgency) — DONE / WON'T FIX

**14. LEGAL filter perf optimization** (mg #4) — DONE faec2e0
**15. Promotion-rank zero in psq init** (zb #2) — DONE 85e9ac4
**16. EP-evasion code-path unification** (mg #9) — DONE 23c86ec (unified
     the two `if constexpr` branches into one shared body gated by an
     `epValid` bool; perft validated)
**17. `states[MAX_GAME_PLIES]` dynamic list** (uci [2], [34]) — WON'T FIX.
     `MAX_GAME_PLIES = 1024` is more than enough for any practical game
     (longest tournament game ever was ~270 plies). Dynamic allocation
     would require lifetime audit of every caller — no measurable benefit.
**18. UCI_Chess960** (uci [25]) — DONE c241285. Hypersion already had
     the working pieces (FEN parser accepts X-FEN, movegen + do_move
     handle arbitrary rook squares, parse_uci_move accepts king-takes-
     rook input); commit c241285 added the missing OUTPUT half: `Position
     ::is_chess960()` detector, `fen()` Shredder-format output,
     `move_uci(Move, const Position&)` king-takes-rook castling output,
     and UCI_Chess960 option no-op-but-acknowledge. Verified: FEN
     round-trips correctly for both standard (KQkq) and Chess960 (HFhf
     etc.) castling rights; standard perft unchanged.
**19. rule50≥96 TT-cutoff re-probe** (search #16) — TESTED 2026-05-17,
     REJECTED. Simple `rule50_count() < 96` gate on TT cutoff: 200g
     bullet (5+0.05, conc=6) = -3.5 ± 38.3 ELO (62W-64L-74D). The
     correctness benefit (rare pathological 50-move-rule positions)
     doesn't outweigh the search-speed loss from disabling cutoffs in
     all rule50≥96 nodes. SF's full version pairs the gate with a
     depth-8 ttMove verification probe (lines 782-796); porting that
     pair would be a follow-up if anyone retries. Logs at
     testing/sprt_search16_*_20260517_*.{log,pgn}.

**20. Malus split SPSA infrastructure** (#116 follow-up) — DONE 62d4442.
     Added `HIST_MALUS_DEPTH2`, `HIST_MALUS_DEPTH1`, `HIST_MALUS_CAP`,
     `HIST_MALUS_CONST` tunables (UCI `setoption name Tune_HIST_MALUS_*`)
     and a `history_malus(depth)` helper. Defaults equal the bonus
     formula so launch behavior is bit-identical until SPSA moves them.

**21. SPSA campaign for malus split** (#116 magnitude tune) — TESTED
     2026-05-17, REJECTED. SPSA at nodes=50000 conc=4 over 200 iters
     converged to (DEPTH2=17, DEPTH1=27, CAP=2208, CONST=18), small
     shift from defaults (16, 30, 2065, 16). Stage 1 triage 30g
     5+0.05 conc=6: +82.6 ± 121.4 (PROMISING). Stage 2 confirm 200g:
     **−19.1 ± 40.3 ELO (64W-75L-61D)** → REJECT. Classic SPSA-at-
     nodes-to-TC non-transfer (the same pattern CLAUDE.md documents
     for the A4 time-mgmt campaign). Defaults reverted; the four
     `Tune_HIST_MALUS_*` UCI hooks stay live so a future TC-mode
     SPSA (testing/spsa.py --tc 5+0.05 instead of --nodes 50000) can
     retry. SF's 1.6x malus_cap:bonus_cap ratio specifically did NOT
     transfer — Hypersion's local optimum at the nodes=50000 horizon
     sits near the bonus cap, not 1.6x above it. Logs:
     testing/sprt_spsa_malus_*_20260517_*.{log,pgn}.

---

## 6th audit pass (2026-05-17) — deep dive on remaining files

Five parallel explore agents scanned nnue.cpp, history.h/movepick.cpp,
tt.cpp/tt.h, timeman/evaluate, and Makefile+headers. ~80 candidate
findings raised; after manual verification against actual source,
**4 actionable real divergences** identified (rest were either DESIGN
divergences, items already in AUDIT.md, or agent over-flagging).

### Real divergences from 6th pass

| # | Location | SF18 ref | Finding | Verdict |
|---|---|---|---|---|
| 6.1 | tt.h:74, tt.cpp:35 | tt.cpp:247 + misc.h:295 | TT cluster index uses lower 32 bits of key only: `uint32_t(key) * clusterCount >> 32`. SF18 uses full 128-bit mul: `mul_hi64(key, clusterCount)`. Hypersion discards upper 32 bits of key entropy. | **REAL** — trivial 1-line fix, low risk |
| 6.2 | tt.cpp:136-138 | tt.cpp:101 | TTEntry::save replacement condition: SF18 adds `+ 2 * pv` PV bonus AND `|| relative_age(generation8)` to the "should replace" predicate. Hypersion has only `depth-4 > existing-4`. | **REAL** — TUNING, low risk |
| 6.3 | movepick.h:24-31 + .cpp:246+ | movepick.cpp:39, 41, 254 | SF18 splits quiets into GOOD_QUIET / BAD_QUIET stages with `partial_insertion_sort(cur, endCur, -3560 * depth)` threshold. Hypersion returns all quiets sorted together via `std::sort()`. | **REAL** — SPRT-required, may regress |
| 6.4 | (everywhere) | types.h | SF18 has `constexpr bool is_valid(Value v)` utility for defensive TT-read sanity checks. Hypersion inlines the equivalent `v != VALUE_NONE` check everywhere. | LATENT — readability only |

### False alarms (agent over-flagging, verified inert)

- NNUE clamp_max recompute — `const __m256i vclamp32` is set OUTSIDE the per-pair loop (line 800), not recomputed.
- minorCorrHist / nonPawnCorrHist missing — search.h:178 documents these were TESTED in 2026-05-12 at -34.9 ELO LTC, reverted. Tables stay declared as dead code for future single-port discipline.
- "Hypersion advantages" (goStartTime, overhead cap, PSQT validation) — already shipped or documented elsewhere.
- ContinuationHistory 3x oversized — agent miscounted dimensions; verify shows [12][64][12][64] is correct 2-deep nesting.

### Confirmed DESIGN divergences (won't fix)

- ButterflyHistory size [2][64][64] vs SF's [2][65536] — Hypersion uses from/to indexing not move.raw(), saves 8x memory at the cost of theoretical signal loss (untested).
- 2-deep ContinuationHistory vs SF's 6-deep — tombstoned negative.
- LowPlyHistory absent — tombstoned -70 ELO @ 30g.
- PawnHistory absent — tombstoned -22 to -49 ELO.
- ExtMove composition vs SF inheritance — both correct.
- TT API: probe-returns-pointer vs SF TTData/TTWriter split — design style.

---

## Plan for 6th-pass actionable items

**Strategy**: ship 6.1 + 6.2 + 6.4 as a low-risk batch (all small, theoretical-positive, no behavior change for 6.4). SPRT validate. Then attempt 6.3 (BAD_QUIET) separately as a higher-variance experiment.

### Batch A (low-risk, ship together)
1. **6.1 TT 64-bit hash**: change `uint64_t(uint32_t(key)) * uint64_t(clusterCount) >> 32` to a 128-bit mul (via `__int128` or `_mulx_u64`). Affects 2 lines (tt.h:74 prefetch, tt.cpp:35 probe).
2. **6.2 TT save PV bonus + age**: add `+ 2*pv` and an aging check to the replacement predicate.
3. **6.4 is_valid helper**: add `constexpr bool is_valid(Value v) noexcept { return v != VALUE_NONE; }` in types.h. No behavior change — cosmetic only.

Expected combined ELO: 0 to +5. Risk: very low.

### Batch B (higher-risk, separate SPRT)
4. **6.3 MovePicker BAD_QUIET**: add GOOD_QUIET / BAD_QUIET stages with depth-scaled threshold. Replicates SF18's quiet-move ordering split.

### Skip
- 6.4-style misc items (RayPassBB, SqrClippedReLU, FT hash warning) — speculative, no clear ELO target.

## 6th-pass SPRT validation (2026-05-17)

Shipped Batch A + Batch B together as a single commit. Tested vs `45b3c0f`
(post-SPSA-revert baseline).

| Stage | TC | Result | Verdict |
|---|---|---|---|
| Stage 1 triage (30g, conc=6) | 5+0.05 | +46.6 ± 110.7 ELO, 13W-9L-8D | NOISE (upper edge) |
| Stage 2 confirm (200g)       | 5+0.05 | **+61.4 ± 40.6 ELO**, 87W-52L-61D | **SHIP** |

CI lower bound +20.8 — comfortably above the +10 SHIP threshold per
PROTOCOL.md. Logs: testing/sprt_sixth_pass_*_20260517_*.{log,pgn}.

The TT 64-bit hash + replacement-PV-bonus + BAD_QUIET stage combine to
~+61 ELO at bullet. Most of the gain likely from BAD_QUIET (better
move ordering at low depth) and TT replacement-priority (PV nodes
linger longer, better PV-line caching across iterations).

## Audit complete

5 audit passes covered every .cpp file in `src/`. **57 fixes applied across
16 commits, all tiers SHIPPED including Tier 3 (qsearch) and Tier 4
(SPSA-sensitive)**.

### Tier 3+4 SPRT validation (2026-05-17)

Commit `3cec85e` was tested as a batch vs `be7adad`:

| Stage | TC | Result | Verdict |
|---|---|---|---|
| Stage 1 triage (30g, conc=6) | 5+0.05 | +70.4 ± 99.8 ELO, 12W-6L-12D | PROMISING |
| Stage 2 confirm (200g) | 5+0.05 | **+79.5 ± 42.8 ELO**, 98W-53L-49D | **SHIP** |
| Stage 3 LTC validation (200g, conc=6) | **60+0.6** | **+68.6 ± 40.1 ELO**, 87W-48L-65D | **NOT TC-SPECIFIC** |

CI lower bound at bullet +36.7, at LTC +28.5 — both well above the +10
SHIP threshold per testing/PROTOCOL.md. Unlike the previous 2026-05-09
session where bullet-shipped changes had ~10:1 bullet:LTC ratio (+178
bullet → +17 LTC), this batch transfers ~1:1 (+79.5 bullet → +68.6 LTC).
The improvements are NOT bullet-specific — they reflect genuine search
quality gains. Logs at testing/sprt_tier34_*_20260517_*.{log,pgn}.

### Structural follow-up tombstone (2026-05-17)

After the Tier 3+4 SHIP, I tried to add the three remaining structural
items as a batch (commit `ec10033`, reverted in `aeb5817`):

- **qs #19** stalemate-detect — `MoveList<LEGAL>` fallback when qsearch
  ends with `bestMove == none && !inCheck`. Correctness fix.
- **#124** alpha-raise-no-cutoff history — added a post-loop history
  bonus block firing when `bestMove != none && bestValue < beta`,
  using `history_bonus(depth)/2` magnitude.
- **#126** fail-low prior-capture bonus — companion to #125, awarding
  captureHist bonus when parent's move was a capture and we fail low.

| Stage | Result | Verdict |
|---|---|---|
| Stage 1 triage (30g, TC 5+0.05, conc=6) | +23.2 ± 98.9 ELO, 10W-8L-12D | NOISE (theoretically positive, proceed) |
| Stage 2a (200g) | +8.7 ± 40.0 ELO, 71W-66L-63D | INCONCLUSIVE |
| Stage 2b (200g, opening offset 200) | +3.5 ± 38.0 ELO, 63W-61L-76D | INCONCLUSIVE |
| Combined 400g | **~+6 ± 28 ELO**, 134W-127L-139D | **REJECT** per PROTOCOL.md (≤ +5 with CI ±35) |

Tombstoned. The Tier 3+4 batch already extracts most of the available
ELO from these audit families — adding more history magnitude on top is
in the noise. qs #19 specifically is a correctness fix with rare impact;
future contributors revisiting it should ablate it alone, not bundled.

Logs: testing/sprt_structural_stage2*_20260517_*.{log,pgn}

Hypersion source is now substantially closer to SF18 semantically while
keeping its intentional architectural divergences (5x eval scale, Fathom
TB backend, corrhist persistence, dual-net switching, AVX2-only NNUE
forward).
