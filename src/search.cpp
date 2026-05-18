// Hypersion — alpha-beta search with PVS, aspiration windows, TT, and the M4
// pruning toolkit:
//   * Reverse Futility Pruning (Static null-move pruning)
//   * Razoring
//   * Null Move Pruning with verification
//   * Internal Iterative Reductions
//   * Late Move Reductions (log-table)
//   * Late Move Pruning
//   * Futility Pruning
//   * SEE-based pruning of bad captures and quiets
//   * Histories: butterfly + capture + counter-move + continuation (1-ply, 2-ply)

#include "search.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "syzygy.h"
#include "tt.h"

namespace hypersion::Search {

ThreadPool Threads;

// LMR reduction table — Reductions[depth][moveCount] = base log reduction (in 1/1024ths
// for fractional precision; we round per call). Filled in init().
namespace {
constexpr int LMR_MAX_DEPTH = 64;
constexpr int LMR_MAX_MOVE  = 64;
int Reductions[LMR_MAX_DEPTH + 1][LMR_MAX_MOVE + 1];

inline int lmr_base(int depth, int moveCount) {
    int d = std::min(depth, LMR_MAX_DEPTH);
    int m = std::min(moveCount, LMR_MAX_MOVE);
    return Reductions[d][m];
}
}  // namespace

void init() {
    if (TT.hashfull() == 0) TT.resize(16);   // default 16 MB until UCI overrides
    for (int d = 0; d <= LMR_MAX_DEPTH; ++d)
        for (int mc = 0; mc <= LMR_MAX_MOVE; ++mc) {
            // Stockfish-style: ~ log(d) * log(mc) / 2 plies. SF18 master uses
            // 1.85 directly; Hypersion v2.0 settled at 1.90 after history.
            // Sweep:
            //   1.95 -> 1.90: +59.6 ELO @ initial baseline (shipped as v2.0)
            //   1.85 vs 1.90: +3.5 +/- 36.8 ELO @ 200g (within noise)
            //   1.85 vs 1.87: +20.9 +/- 39.3 ELO @ 200g 5+0.05 conc=6
            //     (72W-60L-68D, NEW-as-White 54.6 %, NEW-as-Black 50.5 %,
            //      no color asymmetry) — SHIPPED 2026-05-15.
            //   LTC validation at TC 20+0.2 conc=6, 100g:
            //     -20.9 +/- 55.8 ELO (30W-36L-34D, NEW-as-White 44.0 %,
            //      NEW-as-Black 51.0 % — Black asymmetry REVERSED from
            //      bullet). v18 is bullet-specific; at LTC the smaller
            //      divisor produces too-shallow reductions for the deeper
            //      search trees. CI includes 0 but trends negative.
            //      Bullet ship retained (lichess bot plays bullet); future
            //      contributors may want a TC-adaptive divisor.
            //   1.87 vs 1.89: 0.0 +/- 109.8 ELO @ 30g triage (11W-11L-8D)
            //     — no signal, kept 1.87.
            //   1.86 vs 1.87: 0.0 +/- 104.4 ELO @ 30g triage (10W-10L-10D)
            //     — no signal, kept 1.87. Optimum is 1.87 (or in the
            //     [1.85, 1.87] interval but 1.86 is indistinguishable).
            // 1.87 puts us between SF18 master (1.85) and Hypersion v2.0
            // (1.90), in the previously-unexplored interior of the sweep
            // window. Slightly less reduction than 1.85 (smaller divisor
            // gives larger reduction; 1.87 > 1.85 means LESS reduction,
            // i.e. SHALLOWER cuts on quiet moves at high move-count).
            Reductions[d][mc] = (d == 0 || mc == 0) ? 0
                              : int(std::log(double(d)) * std::log(double(mc)) / 1.87);
        }
    Threads.set_size(2);
}
void shutdown() { Threads.stop_all(); Threads.wait_all(); Threads.set_size(0); }

// Forward decl — definition below the tunables namespace.
namespace tunables {
extern int RFP_MARGIN_PER_DEPTH, RAZOR_MARGIN_BASE, RAZOR_MARGIN_PER_DEPTH,
           FUTIL_MARGIN_PER_DEPTH, FUTIL_MARGIN_BASE,
           SEE_QUIET_MARGIN, SEE_CAPT_MARGIN,
           NMP_EVAL_BETA_DIV, PROBCUT_MARGIN,
           ASPIRATION_DELTA0, STABILITY_SWING_TH, QSEARCH_CAP_GAIN;
// History / move-ordering params (A2 SPSA campaign 2026-05-08+).
// Defaults match pre-A2 hardcoded values (16, 32, 2000, 100, 100, 50)
// so behavior is bit-identical at launch and SPSA can move outward.
extern int HIST_BONUS_DEPTH2, HIST_BONUS_DEPTH1, HIST_BONUS_CAP,
           BFLY_WEIGHT, CONT1_WEIGHT, CONT2_WEIGHT;
// Malus split (#116 follow-up).
extern int HIST_MALUS_DEPTH2, HIST_MALUS_DEPTH1, HIST_MALUS_CAP, HIST_MALUS_CONST;
// Time-mgmt scales (A4 SPSA campaign 2026-05-08+). Stored as percent
// multipliers (×/100); defaults preserve the previously hardcoded
// 1.6/1.4/1.2/0.4/0.6/0.85 floats bit-identically.
extern int TM_ENDGAME_BONUS_8, TM_ENDGAME_BONUS_12, TM_ENDGAME_BONUS_16,
           TM_EASY_GAP150, TM_EASY_GAP80, TM_EASY_GAP40;
// A5: previously-hardcoded threat-by-lesser bonuses (movepick.cpp) +
// LMR statScore divisor (search.cpp). Defaults preserve pre-A5
// behavior. Read directly from the namespace at hot-path call sites.
extern int THREAT_BY_LESSER_PENALTY, THREAT_BY_LESSER_BONUS,
           LMR_STATSCORE_DIV;
// A6: history.h gravity-formula constants. HIST_MAX is the saturation
// cap used in update_history's gravity update; HIST_BONUS_CONST is
// the +16 constant offset in history_bonus(d). Defaults preserve
// pre-A6 behavior bit-identically.
extern int HIST_MAX, HIST_BONUS_CONST;
// A7: aspiration window growth + full-window-fallback threshold.
// ASP_GROWTH_ADD is the additive constant in `delta += delta/4 + N`
// when an aspiration search fails. ASP_FULL_WINDOW_TH is the delta
// value at which we abandon aspiration and search with a full window.
extern int ASP_GROWTH_ADD, ASP_FULL_WINDOW_TH;
// A8: NNUE eval-mixing constants. Read from nnue.cpp's forward()
// where the per-net psqt/positional split + material scaling are
// composed into the final cp value.
extern int PSQT_WEIGHT, POSITIONAL_WEIGHT, MATERIAL_SCALE_BASE;
// A10: per-iteration time-scale tunables. FALLING_EVAL_DIV is the
// divisor in `fallingEval = 1.0 + drop / FALLING_EVAL_DIV`. EFFORT_TH
// is the percentile (×100000) above which the best-move effort
// scaling fires. EFFORT_SCALE is the multiplier (÷100) applied when
// effort is concentrated on the best move.
extern int FALLING_EVAL_DIV, EFFORT_TH, EFFORT_SCALE;
// A11: stable-iter time scales (÷100 for fractional). 50 = 0.5x when
// stableIters >= 4; 75 = 0.75x when stableIters >= 2.
extern int STABLE_HIGH_SCALE, STABLE_LOW_SCALE;
}

bool set_tunable(const std::string& name, int value) {
    using namespace tunables;
    if      (name == "RFP_MARGIN_PER_DEPTH")    RFP_MARGIN_PER_DEPTH    = value;
    else if (name == "RAZOR_MARGIN_BASE")       RAZOR_MARGIN_BASE       = value;
    else if (name == "RAZOR_MARGIN_PER_DEPTH")  RAZOR_MARGIN_PER_DEPTH  = value;
    else if (name == "FUTIL_MARGIN_PER_DEPTH")  FUTIL_MARGIN_PER_DEPTH  = value;
    else if (name == "FUTIL_MARGIN_BASE")       FUTIL_MARGIN_BASE       = value;
    else if (name == "SEE_QUIET_MARGIN")        SEE_QUIET_MARGIN        = value;
    else if (name == "SEE_CAPT_MARGIN")         SEE_CAPT_MARGIN         = value;
    else if (name == "NMP_EVAL_BETA_DIV")       NMP_EVAL_BETA_DIV       = value;
    else if (name == "PROBCUT_MARGIN")          PROBCUT_MARGIN          = value;
    else if (name == "ASPIRATION_DELTA0")       ASPIRATION_DELTA0       = value;
    else if (name == "STABILITY_SWING_TH")      STABILITY_SWING_TH      = value;
    else if (name == "QSEARCH_CAP_GAIN")        QSEARCH_CAP_GAIN        = value;
    // A2 history tunables.
    else if (name == "HIST_BONUS_DEPTH2")       HIST_BONUS_DEPTH2       = value;
    else if (name == "HIST_BONUS_DEPTH1")       HIST_BONUS_DEPTH1       = value;
    else if (name == "HIST_BONUS_CAP")          HIST_BONUS_CAP          = value;
    else if (name == "BFLY_WEIGHT")             BFLY_WEIGHT             = value;
    else if (name == "CONT1_WEIGHT")            CONT1_WEIGHT            = value;
    else if (name == "CONT2_WEIGHT")            CONT2_WEIGHT            = value;
    // Malus split (#116 follow-up).
    else if (name == "HIST_MALUS_DEPTH2")       HIST_MALUS_DEPTH2       = value;
    else if (name == "HIST_MALUS_DEPTH1")       HIST_MALUS_DEPTH1       = value;
    else if (name == "HIST_MALUS_CAP")          HIST_MALUS_CAP          = value;
    else if (name == "HIST_MALUS_CONST")        HIST_MALUS_CONST        = value;
    // A4 time-mgmt tunables.
    else if (name == "TM_ENDGAME_BONUS_8")      TM_ENDGAME_BONUS_8      = value;
    else if (name == "TM_ENDGAME_BONUS_12")     TM_ENDGAME_BONUS_12     = value;
    else if (name == "TM_ENDGAME_BONUS_16")     TM_ENDGAME_BONUS_16     = value;
    else if (name == "TM_EASY_GAP150")          TM_EASY_GAP150          = value;
    else if (name == "TM_EASY_GAP80")           TM_EASY_GAP80           = value;
    else if (name == "TM_EASY_GAP40")           TM_EASY_GAP40           = value;
    // A5 threat / LMR-statScore tunables.
    else if (name == "THREAT_BY_LESSER_PENALTY") THREAT_BY_LESSER_PENALTY = value;
    else if (name == "THREAT_BY_LESSER_BONUS")  THREAT_BY_LESSER_BONUS  = value;
    else if (name == "LMR_STATSCORE_DIV")       LMR_STATSCORE_DIV       = value;
    // A6 history-gravity constants.
    else if (name == "HIST_MAX")                HIST_MAX                = value;
    else if (name == "HIST_BONUS_CONST")        HIST_BONUS_CONST        = value;
    // A7 aspiration window tunables.
    else if (name == "ASP_GROWTH_ADD")          ASP_GROWTH_ADD          = value;
    else if (name == "ASP_FULL_WINDOW_TH")      ASP_FULL_WINDOW_TH      = value;
    // A8 NNUE eval-mixing tunables.
    else if (name == "PSQT_WEIGHT")             PSQT_WEIGHT             = value;
    else if (name == "POSITIONAL_WEIGHT")       POSITIONAL_WEIGHT       = value;
    else if (name == "MATERIAL_SCALE_BASE")     MATERIAL_SCALE_BASE     = value;
    // A10 per-iteration time-scale tunables.
    else if (name == "FALLING_EVAL_DIV")        FALLING_EVAL_DIV        = value;
    else if (name == "EFFORT_TH")               EFFORT_TH               = value;
    else if (name == "EFFORT_SCALE")            EFFORT_SCALE            = value;
    // A11 stable-iter time-scales.
    else if (name == "STABLE_HIGH_SCALE")       STABLE_HIGH_SCALE       = value;
    else if (name == "STABLE_LOW_SCALE")        STABLE_LOW_SCALE        = value;
    else return false;
    return true;
}

namespace {

// Hypersion's NNUE eval lives at roughly 5x Stockfish's "1 pawn = 100 cp"
// scale internally. All search constants (RFP, razoring, futility, SEE, NMP,
// ProbCut, aspiration, qsearch, contempt) are tuned to that magnitude — we
// don't touch them.  But UCI tooling (lichess analysis, eval bars, tournament
// software, broadcasts, the human reading `info ... score cp X`) expects the
// SF "1 pawn = 100 cp" convention. So we translate ONLY at the output
// boundary: divide eval by 5 when emitting `cp`. Internal search behavior is
// bit-identical to before this change. Mate scores are unitless so they pass
// through unchanged.
//
// Two prior sessions tried to scale the eval INTERNALLY (Path A v1 / v2) and
// both regressed (-29.6 ELO and -170 ELO). The internal magnitude is bound
// up with the empirically-tuned pruning constants; any change to it without
// a full retune breaks performance. This output-only conversion has zero
// search-side risk — bench is unchanged.
constexpr int OUTPUT_CP_DIVISOR = 5;

std::string score_to_uci(Value v) {
    std::ostringstream ss;
    if (std::abs(v) >= VALUE_MATE_IN_MAX_PLY) {
        int mateMoves = (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;
        ss << "mate " << mateMoves;
    } else {
        ss << "cp " << (int(v) / OUTPUT_CP_DIVISOR);
    }
    return ss.str();
}

std::string move_uci(Move m) {
    if (m == Move::none()) return "(none)";
    if (m == Move::null()) return "0000";
    Square from = m.from_sq(), to = m.to_sq();
    std::string s;
    s += char('a' + file_of(from));
    s += char('1' + rank_of(from));
    s += char('a' + file_of(to));
    s += char('1' + rank_of(to));
    if (m.type_of() == MT_PROMOTION) {
        constexpr char promoChar[] = " pnbrqk";
        s += promoChar[m.promotion_type()];
    }
    return s;
}

// 2026-05-17 audit uci [25] Chess960: emit king-takes-rook castling
// notation when the position is in Chess960 mode. Standard chess
// positions still get "e1g1" / "e8g8" etc.; Chess960 positions emit
// "e1h1" (or whatever file the rook sits on) so Chess960-aware GUIs
// parse it unambiguously.
std::string move_uci(Move m, const Position& pos) {
    if (m == Move::none()) return "(none)";
    if (m == Move::null()) return "0000";
    Square from = m.from_sq(), to = m.to_sq();
    if (m.type_of() == MT_CASTLING && pos.is_chess960()) {
        bool kingSide = file_of(to) == FILE_G;
        Color us = pos.side_to_move();
        // The MT_CASTLING move stored king-to-G/C as `to`; for Chess960
        // output we override `to` with the rook's actual square.
        // Note: this is BEFORE do_move (move not yet played), so
        // castlingRookSquare still has the rook's pre-move file.
        CastlingRights cr = (us == WHITE) ? (kingSide ? WHITE_OO : WHITE_OOO)
                                          : (kingSide ? BLACK_OO : BLACK_OOO);
        to = pos.castling_rook_square(cr);
    }
    std::string s;
    s += char('a' + file_of(from));
    s += char('1' + rank_of(from));
    s += char('a' + file_of(to));
    s += char('1' + rank_of(to));
    if (m.type_of() == MT_PROMOTION) {
        constexpr char promoChar[] = " pnbrqk";
        s += promoChar[m.promotion_type()];
    }
    return s;
}

void update_pv(PVLine& parent, Move m, const PVLine& child) {
    parent.moves[0] = m;
    int n = std::min(child.length, PV_BUFFER - 1);
    for (int i = 0; i < n; ++i)
        parent.moves[i + 1] = child.moves[i];
    parent.length = n + 1;
}

// Margins for various pruning heuristics.
// Phase 9: NNUE eval is in raw SF cp magnitude (avg |v| ~ 636) without the
// previous /3 attenuation. All magnitude-sensitive constants are scaled by 3
// from the original classical-Texel-tuned values so the pruning logic still
// represents the same number of pawns of margin. Note: history bonuses and
// LMR depth-based reductions are NOT scaled — they're not eval-magnitude.
// SPSA-tunable search constants — converted from `constexpr int` to plain
// `int` so UCI options (Tune_*) can perturb them at runtime for SPSA-style
// parameter tuning. Costs ~1-2% NPS over compile-time-folded constants
// (no longer eligible for constant propagation), accepted in exchange for
// runtime tunability without rebuild. After SPSA campaign converges,
// freeze optima back into these defaults.
//
// NOT in anonymous namespace — needs external linkage so the UCI handler
// in uci.cpp can write to them via Search::set_tunable(). External-linkage
// is contained in `namespace tunables { ... }` to keep call sites readable.
}  // close existing anonymous namespace
namespace tunables {

// 12 search-margin constants — pre-A3 (post-individual-sweep) values shown
// inline; A3 SPSA-tuned defaults below shifted these by <2 % each but the
// joint effect is +33 ELO @ 600g (see A3 tombstone block at bottom of namespace).
int RFP_MARGIN_PER_DEPTH    = 240;    // 2026-05-12 SPSA campaign regressed, reverted.
                                       // 2026-05-14 Joint A+B SPSA also regressed (-20.9 ELO @ 200g
                                       // nodes=50000): tried 202; SPRT REJECTED. Reverted to 240.
    // Sweep: 200 = -8.1 ELO at 129g; 280 = -15.6 ELO at 200g.
int RAZOR_MARGIN_BASE       = 852;    // A3: 850 -> 852. Sweep history:
    // 600 = 0.0 +/- 36.4 ELO; 850 = +3.5 +/- 38.3 (kept pre-A3);
    // 750 = -8.7 +/- 37.9 ELO @ 200g 5+0.05 conc=6 (v22, 30g triage was
    //   +11.6 ELO -- classic fakeout, Stage 2 reverted to mild negative).
int RAZOR_MARGIN_PER_DEPTH  = 383;    // A9 joint: 387 -> 383. A3 was: 390 -> 387.
    // Sweep history: 300 = -34.9 ELO @ 30g; 480 = +46.6 @ 30g but -51.6 @ 61g.
int FUTIL_MARGIN_PER_DEPTH  = 397;    // Joint A+B SPSA tried 400 (no-op-ish), REJECTED. Sweep history:
    // 330 -> 400 was +15.6 +/- 37.6 ELO @ 200g;
    // 410 = -58.5 +/- 108.4 ELO @ 30g (v25, REJECT at triage -- noise band
    // but ≤ -50 lower bound, don't proceed to Stage 2).
int FUTIL_MARGIN_BASE       = 385;    // A3: 390 -> 385. Sweep history:
    // 480 = -23.2 ELO; 300 = -46.6 ELO. Both directions worse than 390.
int SEE_QUIET_MARGIN        = -181;   // Joint A+B SPSA tried -155, REJECTED. Sweep history:
    // -150 = -70 ELO; -220 = -1.7 +/- 38.5 ELO;
    // -200 = -15.6 +/- 37.2 ELO @ 200g (v23 30g triage was +23.2 -- classic
    //   fakeout, Stage 2 reverted to mild negative with Black asymmetry).
int SEE_CAPT_MARGIN         = -252;   // Joint A+B SPSA tried -246, REJECTED. Sweep history:
    // -400 = -58 ELO; -250 = +8.7 +/- 39.7 ELO vs -300 baseline.
int NMP_EVAL_BETA_DIV       = 803;    // 2026-05-12 SPSA campaign regressed, reverted.
                                       // 2026-05-14 Joint A+B SPSA tried 859, REJECTED (-20.9 ELO).
    // 600 -> 800 = +8.7 ELO; 800 -> 1200 = -1.7 ELO.
int PROBCUT_MARGIN          = 802;    // 2026-05-12 SPSA campaign regressed, reverted.
                                       // 2026-05-14 Joint A+B SPSA tried 757, REJECTED (-20.9 ELO).
    //   500: -24.4 ELO; 600: baseline; 800: +22.6 ELO (shipped); 1000: -45.4.
int ASPIRATION_DELTA0       =  50;    // A3: 51 -> 50. Sweep history:
    // 30 = -10.4 ELO; 80 = +1.7 ELO; 65 = -34.9 +/- 101.9 @ 30g (noise, neg).
int STABILITY_SWING_TH      =  61;    // 2026-05-12 SPSA campaign (8 params, 1600g) regressed -13.9 ELO @ 200g. Reverted.
    // 100 / 40 both regress vs 60.
int QSEARCH_CAP_GAIN        = 3221;   // A9 joint: 3259 -> 3221. A3 was: 3300 -> 3259.
    // Sweep history: 2200 = -209 ELO @ 13g; 5000 = 0.0 ELO @ 30g.

// ---- A2 history / move-ordering tunables (2026-05-08+) ----
// SPSA-tuned via the v2 campaign (200 iters x 64 games/iter,
// nodes=50000, conc=6, ~30 min wall, seed=2). Each constant exposed
// at runtime via UCI Tune_<NAME> for future re-tuning campaigns.
//
// HIST_BONUS_DEPTH2 / DEPTH1 / CAP are the coefficients in
//   history_bonus(depth) = min(CAP, DEPTH2*d^2 + DEPTH1*d + 16)
// (history.h). Used everywhere we apply a history update.
//
// BFLY_WEIGHT / CONT1_WEIGHT / CONT2_WEIGHT are percent multipliers
// (×/100) applied to butterfly, 1-ply contHist, and 2-ply contHist
// reads in MovePicker::score_quiets (movepick.cpp).
//
// A2 v1 campaign (rejected, 2026-05-08):
//   16 games/iter, ~5% step sizes. Per-iter |y| stayed at the
//   1/16=0.062 noise floor. Converged to BFLY=106, CONT1=102,
//   CONT2=48, BONUS_D2=17, BONUS_D1=32, CAP=1935.
//   200g vs defaults: -34.9 +/- 35.1 ELO (REJECT, fakeout from
//   +58.5 ELO @ 30g).
//
// A2 v2 campaign (SHIPPED, 2026-05-08):
//   64 games/iter (4x cleaner gradient — noise floor 1/64=0.016),
//   step sizes 15-25 % of range (4x wider). 200 iters, ~30 min
//   wall. Converged to values barely changed from defaults:
//     BFLY 100->101, CONT1 100->99, CONT2 50->47,
//     BONUS_D2 16 (unchanged), BONUS_D1 32->30, CAP 2000->2059.
//   Tested vs defaults @ 5+0.05, conc=6, two independent 200g runs:
//     run 1: +26.1 +/- 39.1 ELO  (73-58-69)
//     run 2: +29.6 +/- 39.8 ELO  (76-59-65)
//     combined 400g: +27.9 +/- ~17 ELO  (149-117-134, score 0.540)
//   Two independent confirms agreed within 4 ELO -> ship.
//
// Lesson learned: SPSA on Hypersion's history landscape needs
// 64+ g/iter to surface signal. The v1 16-g/iter campaign random-
// walked because its noise floor masked the gradient. With proper
// statistical power, the v2 campaign found a small but real
// improvement region (~1-6 % shifts) worth +28 ELO.
int HIST_BONUS_DEPTH2 = 16;     // unchanged
int HIST_BONUS_DEPTH1 = 30;     // A2-v2: was 32 (A9 confirmed)
    // v29 trial 31: 0.0 +/- 39.5 ELO @ 200g (67W-67L-66D) -- exact-zero
    // noise. Per protocol REJECT. Interior point doesn't move needle.
int HIST_BONUS_CAP    = 2065;   // A9 joint: 2059 -> 2065. A2-v2 was: 2000 -> 2059.
    // v26 trial 2080: +1.7 +/- 36.3 ELO @ 200g (57W-56L-87D, NEW-as-Black
    // 41.9 %). Per protocol REJECT (≤ +5 with CI ±35-36). 30g triage was
    // +58.5 -- classic fakeout. SPSA trend ceiling at 2065.
// ---- Malus split (#116 follow-up, 2026-05-17) ----
// SF18 src/search.cpp uses separate bonus / malus formulas with malus
// ~1.6x bonus_cap. Hypersion's malus currently equals bonus magnitude;
// the moveCount taper (shipped in 3cec85e) is applied on top.
//
// 2026-05-17 SPSA campaign (200 iters x 4 games x nodes=50000, conc=4)
// converged to (17, 27, 2208, 18). Stage 1 triage 30g 5+0.05 looked
// promising (+82.6 +/- 121.4 ELO) but Stage 2 200g confirmed REJECT
// at -19.1 +/- 40.3 ELO (64W-75L-61D). Classic SPSA-at-fixed-nodes
// non-transfer to TC-mode play (the same pattern CLAUDE.md documents
// for the A4 time-mgmt SPSA campaign). Defaults reverted to
// bonus-formula bit-identical values; the four Tune_HIST_MALUS_*
// hooks remain so a future TC-mode SPSA (testing/spsa.py --tc 5+0.05
// instead of --nodes 50000) can retry. SF's 1.6x ratio specifically
// did NOT transfer — Hypersion's local optimum sits near the bonus
// cap, not 1.6x above it.
int HIST_MALUS_DEPTH2 = 16;
int HIST_MALUS_DEPTH1 = 30;
int HIST_MALUS_CAP    = 2065;
int HIST_MALUS_CONST  = 16;
int BFLY_WEIGHT       = 101;    // A2-v2: was 100 (A9 confirmed)
int CONT1_WEIGHT      =  99;    // A2-v2: was 100 (A9 confirmed)
int CONT2_WEIGHT      =  48;    // A9 joint: 47 -> 48. A2-v2 was: 50 -> 47.
    // v28 trial 49: +1.7 +/- 39.7 ELO @ 200g (68W-67L-65D) -- noise.
    // Per protocol REJECT (≤ +5 with CI ±35-40). Restored to 48.

// ---- A4 time-management tunables (2026-05-08+) ----
// Stored as percent multipliers (value / 100). Defaults preserve the
// previously-hardcoded floats bit-identically.
//
// TM_ENDGAME_BONUS_8/12/16: extra time spent at low piece counts.
//   Apply: scale *= (TM_ENDGAME_BONUS_X / 100.0) when totalPieces <= X.
// TM_EASY_GAP150/80/40: easy-move time savers when root[0] beats
//   root[1] by at least 150/80/40 cp AND stableIters >= 3.
//   Apply: scale = min(scale, TM_EASY_GAP_X / 100.0).
//
// A4 SPSA campaign (2026-05-08, 200 iters x 64 games/iter) tombstone:
//   Even smaller shifts than A3 — only 3 of 6 params moved at all,
//   max delta +3.3 % (TM_ENDGAME_BONUS_16: 120 -> 124).
//   Converged values: 160/140/124/40/61/84.
//   200g SPRT vs default-Tune_* BASE @ 5+0.05, conc=6:
//     run 1: -3.5 +/- 36.1 ELO  (W=55 L=57 D=88, score 0.495)
//   Result is at the noise floor with CI including 0. Did not pursue
//   runs 2-3: tiny shifts + negative point estimate predict tombstone.
//
//   Diagnosis: time-management scales were already well-tuned via the
//   previous manual rebalance experiment (1.6/1.4/1.2 -> 1.4/1.25/1.1)
//   which itself tombstoned at -18 ELO @ 500g. SPSA at the v2/A3
//   methodology can find small joint optima where manual sweeps cannot,
//   but only when the parameter region has actual gradient. For
//   time-mgmt scales the gradient signal is below the 1/64 noise floor
//   even at 64 games/iter — possibly because clock time is a discrete
//   resource and small scale shifts integrate to almost-identical
//   per-move budgets.
//
//   Infrastructure (Tune_* + plumbing) stays SHIPPED. Defaults frozen
//   at pre-A4 values. Future contributor wanting to retry should pair
//   with TC variation (test at multiple TCs in one campaign) or move
//   to integer-resource-aware SPSA where a 1-unit scale change has
//   a deterministic move-budget consequence.
//
// A4 LTC retry (2026-05-11, 100 iters x 4 games/iter, nodes=500000):
//   Same 6 time-mgmt + 2 history params (the latter from A6) at LTC
//   node budget. Convergence shifts of 6-27% from defaults:
//     TM_ENDGAME_BONUS_8:  160 -> 179 (+11.9%)
//     TM_ENDGAME_BONUS_12: 140 -> 128 (-8.6%)
//     TM_ENDGAME_BONUS_16: 120 -> 110 (-8.3%)
//     TM_EASY_GAP150:       40 -> 34  (-15.0%)
//     TM_EASY_GAP80:        60 -> 76  (+26.7%)
//     TM_EASY_GAP40:        85 -> 76  (-10.6%)
//     HIST_MAX:           7183 -> 6433 (-10.4%)
//     HIST_BONUS_CONST:     16 -> 15   (-6.2%)
//   100g LTC SPRT vs default-Tune_* BASE @ 60+0.6, conc=6:
//     +0.0 +/- 53.1 ELO  (W=30 L=30 D=40, score 0.500)
//   Perfectly neutral — TOMBSTONE. Even with larger parameter shifts
//   than A4 bullet, the LTC time-management gradient is below the
//   ~30 ELO noise floor at this sample size. Confirms the bullet
//   finding: these knobs are at a local optimum at all TCs.
int TM_ENDGAME_BONUS_8  = 160;   // pre-A4: 1.6, A4 unchanged
int TM_ENDGAME_BONUS_12 = 140;   // pre-A4: 1.4, A4 unchanged
int TM_ENDGAME_BONUS_16 = 120;   // pre-A4: 1.2, A4 unchanged (defaults preserved)
int TM_EASY_GAP150      =  40;   // pre-A4: 0.4, A4 unchanged
int TM_EASY_GAP80       =  60;   // pre-A4: 0.6, A4 unchanged
int TM_EASY_GAP40       =  85;   // pre-A4: 0.85, A4 unchanged

// ---- A5 threat-by-lesser + LMR statScore (2026-05-09) ----
// Three previously-hardcoded constants. SPSA-tuned via the A5 v2/A3
// methodology (200 iters x 64 g/iter); LMR_STATSCORE_DIV shifted
// 8192 -> 8063 (-1.6 %), both threat-by-lesser bonuses unchanged.
//
// THREAT_BY_LESSER_PENALTY: bonus applied when moving a piece TO a
//   square attacked by a strictly-lesser-valued enemy piece. Negative
//   value (penalty). Multiplied by PieceValueMG[pt] in score_quiets.
// THREAT_BY_LESSER_BONUS: bonus applied when moving a piece AWAY from
//   such a threat. Positive value. Multiplied by PieceValueMG[pt].
// LMR_STATSCORE_DIV: divisor for the LMR statScore correction in
//   search.cpp. Higher values reduce the magnitude of history-driven
//   reduction adjustments; lower values amplify them.
//
// A5 SPRT vs default-Tune_* BASE @ 5+0.05, conc=6, three independent
// 200g runs (cutechess re-randomizes openings per match):
//   run 1: +17.4 +/- 38.0 ELO  (W=67 L=57 D=76)
//   run 2: +26.1 +/- 38.8 ELO  (W=72 L=57 D=71)
//   run 3: +72.2 +/- 38.2 ELO  (W=82 L=41 D=77)
//   combined 600g: +38.4 ELO with 95 % CI (+16.7, +61.1)
//                  (W=221 L=155 D=224, score 0.555)
//
// All three runs positive, lower CI bound +16.7 > +10. Same lesson
// confirmed yet again: tiny SPSA shifts (here just one -1.6 % move)
// can compose into ~+38 ELO when applied to a previously-untuned
// continuous parameter region.
int THREAT_BY_LESSER_PENALTY = -19;    // unchanged from pre-A5
int THREAT_BY_LESSER_BONUS   =  20;    // unchanged from pre-A5
int LMR_STATSCORE_DIV        = 7938;   // 2026-05-12 SPSA campaign regressed, reverted.
                                        // 2026-05-14 Joint A+B SPSA tried 8488, REJECTED.

// ---- A6 history.h gravity constants (2026-05-09) ----
// HIST_MAX: saturation cap on history values. Used as both a clamp
//   on incoming bonus AND the divisor in the gravity update formula
//   `entry += bonus - entry * |bonus| / HIST_MAX`. SF tuned 7183.
// HIST_BONUS_CONST: the +16 constant offset in history_bonus(d).
//   Acts as a floor on small-depth bonuses.
//
// A6 SPSA campaign (2026-05-09, 200 iters x 64 games/iter, ~30 min):
// Convergence was the cleanest "no gradient detectable" signal yet
// — both params returned EXACTLY to defaults:
//     HIST_MAX:         7183 -> 7183  (no movement)
//     HIST_BONUS_CONST:   16 -> 16    (no movement)
// SPSA's random walk had nothing to find. Skipped the 200g SPRT
// (zero parameter shift means zero ELO change vs default-Tune_* BASE).
//
// SF's 7183 cap and the 16 constant offset are at the local optimum
// for Hypersion as well — no surprise given they're inherited from
// Stockfish's heavy SPSA tuning. Infrastructure stays SHIPPED for
// possible future re-attempts (e.g., paired with a stronger NNUE
// where the eval magnitude shift could reopen the history-update
// dynamics).
//
// A6 LTC retry (2026-05-11, 100 iters x 4 games/iter, nodes=500000):
// Joint LTC SPSA with the A4 tunables; HIST_MAX moved 7183 -> 6433
// (-10.4%), HIST_BONUS_CONST 16 -> 15 (-6.2%). 100g LTC SPRT vs
// defaults: +0.0 +/- 53.1 ELO. TOMBSTONE retained.
int HIST_MAX         = 7183;   // SF-tuned, A6 + A6-LTC confirm local optimum
int HIST_BONUS_CONST =   16;   // SF-tuned, A6 + A6-LTC confirm local optimum

// ---- A7 aspiration-window growth (2026-05-09) ----
// ASP_GROWTH_ADD: additive constant in the aspiration delta growth
//   formula `delta += delta/4 + ASP_GROWTH_ADD` after a fail. SF
//   default 5; ranges that make sense are 0-15.
// ASP_FULL_WINDOW_TH: when aspiration delta exceeds this, abandon
//   aspiration and search with full -INF/+INF window. SF default
//   1000; ranges 500-2000.
//
// A7 SPSA campaign (2026-05-09, 200 iters x 64 games/iter, ~30 min):
// Tiny convergence — only ASP_FULL_WINDOW_TH moved (1000 -> 992,
// -0.8 %); ASP_GROWTH_ADD unchanged. Single 200g SPRT vs default-
// Tune_* BASE @ 5+0.05 conc=6:
//   -15.6 +/- 35.6 ELO  (W=50 L=59 D=91, score 0.477)
// Below +5 tombstone threshold. The fall-back-to-full-window
// threshold rarely fires in normal play (aspiration delta rarely
// exceeds 200-300 cp before finding the right window), so the
// movement is functionally near-noop and the SPRT result is
// dominated by random variance.
//
// Tombstoned. Defaults frozen at SF-tuned values. Same pattern as
// A6: SPSA finds nothing on already-well-tuned regions.
int ASP_GROWTH_ADD     =    5;   // SF-tuned, A7 confirmed local optimum
int ASP_FULL_WINDOW_TH = 1000;   // SF-tuned, A7 confirmed local optimum

// ---- A8 NNUE eval-mixing constants (2026-05-09) ----
// PSQT_WEIGHT / POSITIONAL_WEIGHT: the 125/131 mixing constants in
//   `nnue = (PSQT_WEIGHT * pv + POSITIONAL_WEIGHT * pp) / 128`
//   that combine PSQT and positional eval components from NNUE.
// MATERIAL_SCALE_BASE: the 77871 base in
//   `v = (nnue * (MATERIAL_SCALE_BASE + mat)) / MATERIAL_SCALE_BASE`
//   that scales eval magnitude by total material on the board.
// All three from SF18 evaluate.cpp; preserved through Hypersion's
// NNUE port and never re-tuned for Hypersion's specific search.
//
// A8 SPSA campaign (2026-05-09, 200 iters x 64 g/iter, ~30 min):
// Tiny convergence (all 3 params shifted <1 % from defaults):
//   PSQT_WEIGHT         125 -> 126
//   POSITIONAL_WEIGHT   131 -> 130
//   MATERIAL_SCALE_BASE 77871 -> 77308
// SPRT vs default-Tune_* BASE @ 5+0.05, conc=2 (memory-aggressive):
//   run 1: +12.2 +/- 34.6 ELO  (W=55 L=48 D=97, score 0.517)
//   run 2: -33.1 +/- 37.0 ELO  (W=49 L=68 D=83, score 0.453)
//   combined 400g: -10.4 ELO with 95 % CI (-35.5, +14.6)
//
// 45-ELO swing between runs, combined point estimate slightly
// negative. Run 1's +12.2 was a noise fakeout. CI includes zero.
//
// Tombstoned. Defaults frozen at SF-inherited values. NNUE eval
// composition is at SF's heavily-tuned local optimum — same pattern
// as A6/A7. Future contributor wanting to retry should pair with
// either an NNUE retrain (the eval-magnitude calibration could
// shift) or with bigger movements coupled to other tunables.
int PSQT_WEIGHT         =   125;   // SF-tuned, A8 confirmed local optimum
int POSITIONAL_WEIGHT   =   131;   // SF-tuned, A8 confirmed local optimum
int MATERIAL_SCALE_BASE = 77871;   // SF-tuned, A8 confirmed local optimum

// ---- A10 per-iteration time-scale tunables (2026-05-09) ----
// FALLING_EVAL_DIV: divisor in `1.0 + drop/N` for the eval-dropped-
//   between-iters time bump. SF default 1000.
// EFFORT_TH: percentile threshold (×100000) above which effort-scaling
//   fires. SF default 93340 = 93.34 %.
// EFFORT_SCALE: multiplier (÷100) applied when effort is concentrated.
//   SF default 76 = 0.76x. Hypersion previously hardcoded.
//
// A10 SPSA campaign (2026-05-09, 200 iters x 64 g/iter, ~30 min):
// Only FALLING_EVAL_DIV moved meaningfully (1000 -> 1058, +5.8 %).
// EFFORT_TH and EFFORT_SCALE both essentially unchanged.
//
// SPRT vs default-Tune_* BASE @ 5+0.05, conc=6:
//   run 1: +43.7 +/- 37.7 ELO  (W=73 L=48 D=79, score 0.563)
//   run 2: +10.4 +/- 35.8 ELO  (W=58 L=52 D=90, score 0.515)
//   combined 400g: +27.2 ELO with 95 % CI (+1.4, +53.0)
//
// Both runs positive, combined point estimate well above +10. Lower
// CI bound +1.4 is borderline (barely above 0), but per the multi-
// run-confirm criteria established earlier in the session, SHIP.
// Higher FALLING_EVAL_DIV means a SMALLER time bump per cp of eval
// drop — Hypersion was over-weighting the falling-eval signal at /1000.
int FALLING_EVAL_DIV =  1058;   // 2026-05-12 SPSA campaign regressed, reverted.
int EFFORT_TH        = 93340;   // SF-tuned, A10 confirmed
int EFFORT_SCALE     =    76;   // SF-tuned, A10 confirmed

// ---- A11 stable-iter time scales (2026-05-09) ----
// STABLE_HIGH_SCALE: applied when stableIters >= 4 (very stable best
//   move across many iterations — short-circuit time use).
// STABLE_LOW_SCALE: applied when stableIters >= 2 (moderately stable).
// Stored as percent (÷100). Defaults preserve pre-A11 0.5 / 0.75.
//
// A11 SPSA campaign (2026-05-09): zero-movement convergence — both
// params returned EXACTLY to defaults. SPSA had nothing to find.
// Skipped the 200g SPRT (zero shift = zero ELO change). Same pattern
// as A6/A7/A8 already-tuned regions. SF's 0.5/0.75 stable-iter
// scales are at Hypersion's local optimum too. Infrastructure stays
// SHIPPED for future re-tuning campaigns.
int STABLE_HIGH_SCALE =  50;   // 2026-05-12 SPSA campaign regressed, reverted.
int STABLE_LOW_SCALE  =  75;   // 2026-05-12 SPSA campaign regressed, reverted.

// SPSA campaign history — DO NOT REPEAT FAILED VARIANTS WITHOUT READING.
//
// A1 campaign (2026-05-07/08) — REJECTED:
//   Above 12 tunables exposed to UCI as `setoption name Tune_<NAME> value <int>`
//   (commit 69a15fa). Two A1 variants run at 4 games/iter:
//     Slow  (TC 5+0.05, conc=2, ~9h):  -75.9 +/- 30 ELO @ 93g (aborted).
//     Fast  (nodes=50000 conc=6, ~2h40m): -8.7 +/- ~36 ELO @ 200g (REJECT).
//   Both campaigns moved parameters by 5-25 % from defaults; both regressed.
//   Diagnosis: 4 games/iter under-resolves Hypersion's flat objective surface
//   — SPSA random-walks instead of finding gradient.
//
// A3 campaign (2026-05-08) — SHIPPED at +33.1 ELO @ 600g:
//   Same 12 params, v2-history-style methodology: 64 games/iter (16x A1's
//   game budget), 15-25 % step sizes (3-4x A1's), nodes=50000, conc=6,
//   200 iters, ~40 min wall, seed=3. Converged to tiny shifts (all <2 %
//   from defaults — A1's aggressive movements were noise-driven).
//   SPRT vs default-Tune_* BASE @ 5+0.05, conc=6, three independent 200g
//   confirms (cutechess re-randomizes openings):
//     run 1: +15.6 +/- 38.2 ELO  (67-58-75)
//     run 2: +59.6 +/- 35.9 ELO  (72-38-90)
//     run 3: +24.4 +/- 36.8 ELO  (65-51-84)
//     combined 600g: +33.1 ELO with 95 % CI (+11.9, +54.6)  (204-147-249)
//   Lower CI bound +11.9 > +10 ship threshold across three independent
//   runs. Defaults updated above.
//
// Lesson confirmed across A1 + A2-v2 + A3: SPSA on Hypersion needs 64+
// games/iter to surface signal. Below that, the 1/N noise floor dominates
// any real gradient. With proper statistical power, even 2 % parameter
// shifts can compose into +33 ELO when found jointly.
//
// Phase 2 / v33 joint A+B retry (2026-05-16) — TOMBSTONE:
//   7 search-margin params (RFP, LMR_STATSCORE, SEE_QUIET, SEE_CAPT,
//   FUTIL, NMP_EVAL_BETA, PROBCUT) at 16 games/iter (the noise-floor
//   middle ground between A1's 4 and A2-v2/A3's 64), 200 iters, TC
//   5+0.05 conc=6. Total 3200 SPSA games, ~2h50m wall, seed=33.
//   Convergence shifts vs defaults:
//     RFP_MARGIN_PER_DEPTH:   240 -> 246  (+2.5%)
//     LMR_STATSCORE_DIV:     7938 -> 7900 (-0.5%)
//     SEE_QUIET_MARGIN:      -181 -> -183 (-1.1%)
//     SEE_CAPT_MARGIN:       -252 -> -250 (+0.8%)
//     FUTIL_MARGIN_PER_DEPTH: 397 -> 386  (-2.8%)
//     NMP_EVAL_BETA_DIV:      803 -> 792  (-1.4%)
//     PROBCUT_MARGIN:         802 -> 803  (+0.1%)
//   All shifts <3 %. Two-run SPRT vs defaults (Tune_*):
//     run 1: +27.9 +/- 38.7 ELO @ 200g  (72-56-72)
//     run 2: -26.1 +/- 39.7 ELO @ 200g  (60-75-65)
//     combined 400g: ~+1 ELO  (132-131-137, dead-even)
//   Two independent runs disagreed in sign — classic SPRT fakeout at
//   1-run sample size. Combined 400g is essentially zero ELO change.
//   Reverted to current defaults. TOMBSTONE.
//
// Insight: 16 g/iter is still noise-dominated for the search-margin
// cluster. The A2-v2 history cluster shipped at 64 g/iter; the same
// statistical power is needed here. Future contributor wanting to
// retry should use 64+ g/iter (~7-8h compute) and double-confirm at
// 200g+200g SPRT before shipping.
//
// Infrastructure (tunables namespace, set_tunable, UCI Tune_* handler)
// stays SHIPPED for future re-tuning campaigns.

}  // namespace tunables
namespace {  // re-open anonymous namespace
using tunables::RFP_MARGIN_PER_DEPTH;
using tunables::RAZOR_MARGIN_BASE;
using tunables::RAZOR_MARGIN_PER_DEPTH;
using tunables::FUTIL_MARGIN_PER_DEPTH;
using tunables::FUTIL_MARGIN_BASE;
using tunables::SEE_QUIET_MARGIN;
using tunables::SEE_CAPT_MARGIN;
using tunables::NMP_EVAL_BETA_DIV;
using tunables::PROBCUT_MARGIN;
using tunables::ASPIRATION_DELTA0;
using tunables::STABILITY_SWING_TH;
using tunables::QSEARCH_CAP_GAIN;
// A4 time-mgmt tunables.
using tunables::TM_ENDGAME_BONUS_8;
using tunables::TM_ENDGAME_BONUS_12;
using tunables::TM_ENDGAME_BONUS_16;
using tunables::TM_EASY_GAP150;
using tunables::TM_EASY_GAP80;
using tunables::TM_EASY_GAP40;
// A5 LMR statScore divisor (threat-by-lesser tunables read inside movepick.cpp).
using tunables::LMR_STATSCORE_DIV;
// A7 aspiration window tunables.
using tunables::ASP_GROWTH_ADD;
using tunables::ASP_FULL_WINDOW_TH;
// A10 per-iter time-scale tunables.
using tunables::FALLING_EVAL_DIV;
using tunables::EFFORT_TH;
using tunables::EFFORT_SCALE;
// A11 stable-iter scales.
using tunables::STABLE_HIGH_SCALE;
using tunables::STABLE_LOW_SCALE;

inline int lmp_threshold(int depth, bool improving) {
    // Stockfish-style movecount threshold: more aggressive when not improving.
    return improving ? (3 + depth * depth) : (3 + depth * depth) / 2;
}

// Detect "shuffling" — engine moving the same piece back and forth
// across recent plies, indicating no progress. Ported from SF master
// (post-SF18). Used to disable singular extension on shuffling moves
// so search budget goes to alternative plans instead of confirming a
// shuffle line. Addresses the lichess H05vgtVr endgame conversion
// failure where Hypersion drew K+R+K by 50-move rule after shuffling
// 40+ moves.
//
// Trigger conditions:
//   - move is not a capture
//   - rule50 counter has accumulated (>= 10)
//   - we're past the opening (ply >= 20, no recent null move)
//   - the move's from-square matches the to-square of (ss-2)'s move
//     AND (ss-2)'s from-square matches (ss-4)'s to-square (the same
//     piece moved back-and-forth across the last 4 of its plies)
inline bool is_shuffling(Move m, const Stack* ss, const Position& pos) {
    if (pos.capture(m) || pos.rule50_count() < 10) return false;
    if (pos.state()->pliesFromNull <= 6 || ss->ply < 20) return false;
    Move prev1 = (ss - 2)->currentMove;
    Move prev2 = (ss - 4)->currentMove;
    if (prev1 == Move::none() || prev1 == Move::null()) return false;
    if (prev2 == Move::none() || prev2 == Move::null()) return false;
    return m.from_sq() == prev1.to_sq()
        && prev1.from_sq() == prev2.to_sq();
}

// SF18 src/search.cpp:127 — `value_draw` randomises the returned draw
// value by 2 cp based on a low bit of nodes searched. Without this, search
// sees draw == exactly 0 and can lock into "alpha == beta == 0" equilibria
// in won positions that lead to 3-fold repetition. The ±1 cp wobble forces
// search to ALWAYS find SOMETHING better than a draw, breaking 3-fold
// blindness. -contempt is folded in so user-set contempt still controls
// how much we want to avoid draws.
inline Value value_draw(std::uint64_t n, int contempt) {
    return Value(int(n & 0x2) - 1 - contempt);   // -1-contempt or +1-contempt
}

// T1.1 (Kirin V8 port) — TOMBSTONED 2026-05-14.
// Tried material-aware draw contempt: when STM materially ahead/behind,
// penalty added to the returned draw value (up to ±50 cp) so search
// avoids settling for draw when winning / accepts draw when losing.
// Result: NNUE bench shifted by -13 nodes (1,273,328 -> 1,273,315) and
// WAC depth-8 dropped 184 -> 182. The change is tiny but non-zero.
// Search apparently relies on the exact draw value to stay at the
// "right" magnitude for alpha-beta windows; small material-driven
// shifts subtly change pruning. Reverted.
// Future contributor: if this experiment is re-tried, gate it on
// limits.contempt > 0 only (user-explicit anti-draw mode), not
// always-on. Or use a much smaller penalty (<= 5 cp).

}  // namespace

// ---------------------------------------------------------------------------
// Worker lifecycle
// ---------------------------------------------------------------------------
Worker::Worker() {
    for (auto& ch : contHist) ch = std::make_unique<ContinuationHistory>();
}
Worker::~Worker() { stop(); wait_for_finish(); }

void Worker::clear() {
    mainHist.clear();
    captureHist.clear();
    killers.clear();
    counterMoves.clear();
    pawnCorrHist.clear();
    materialCorrHist.clear();
    contCorrHist1.clear();
    contCorrHist2.clear();
    threatHist.clear();
    for (auto& ch : contHist) ch->clear();
    // Note: TT clearing is the pool's responsibility — done once across all threads.
    // NOTE: 2026-05-12 added minor + nonpawn-W + nonpawn-B correction histories
    // with SF18 weight blend (10347, 8821, 11665, 11665) → /42498/256 divisor.
    // Bundled with NMP changes for LTC test. LTC 20g cumulative -34.9 ± 111
    // ELO. Reverted. Future retry: add ONE corrhist at a time (just minor, or
    // just nonpawn), not the full bundle. Storage cost: ~512KB per extra
    // table per worker — manageable.
}

void Worker::decay_for_new_game() {
    // Soft reset on `ucinewgame` — keep cross-game learned signal but halve
    // it so a position with very different character isn't dominated by old
    // statistics. Killers stay reset (they're position-specific). Correction
    // history is bounded already, decay is unnecessary there.
    mainHist.decay();
    captureHist.decay();
    killers.clear();
    counterMoves.clear();
    for (auto& ch : contHist) ch->decay();
}

// ── Lc0-inspired persistent correction history I/O ────────────────────────
// File layout (single file holding BOTH tables):
//   4 bytes  magic 'HCP1'   (Hypersion Corr-history Pair v1)
//   4 bytes  size_pawnCH    (bytes in pawnCorrHist.data; must match on load)
//   data     pawnCorrHist.data (~128 KB)
//   4 bytes  size_matCH     (bytes in materialCorrHist.data)
//   data     materialCorrHist.data (~128 KB)
// Failures (missing file / mismatched size / read error) are non-fatal:
// the in-memory tables stay at their previous state.
static constexpr std::uint32_t HCP1_MAGIC = 0x31504348u;   // 'H','C','P','1'

bool Worker::save_corr_hist(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::uint32_t magic = HCP1_MAGIC;
    std::uint32_t sz_pch = std::uint32_t(sizeof(pawnCorrHist.data));
    std::uint32_t sz_mch = std::uint32_t(sizeof(materialCorrHist.data));
    bool ok = std::fwrite(&magic,   4, 1, f) == 1
           && std::fwrite(&sz_pch,  4, 1, f) == 1
           && std::fwrite(pawnCorrHist.data,    1, sz_pch, f) == sz_pch
           && std::fwrite(&sz_mch,  4, 1, f) == 1
           && std::fwrite(materialCorrHist.data, 1, sz_mch, f) == sz_mch;
    std::fclose(f);
    return ok;
}
bool Worker::load_corr_hist(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::uint32_t magic = 0, sz_pch = 0, sz_mch = 0;
    auto read_u32 = [&](std::uint32_t& dst) {
        return std::fread(&dst, 4, 1, f) == 1;
    };
    bool ok = read_u32(magic) && magic == HCP1_MAGIC
           && read_u32(sz_pch) && sz_pch == sizeof(pawnCorrHist.data)
           && std::fread(pawnCorrHist.data, 1, sz_pch, f) == sz_pch
           && read_u32(sz_mch) && sz_mch == sizeof(materialCorrHist.data)
           && std::fread(materialCorrHist.data, 1, sz_mch, f) == sz_mch;
    std::fclose(f);
    if (ok) {
        // Apply one extra halving on load. Cumulative effect: very old data
        // (from many sessions ago) fades faster than per-game decay alone.
        pawnCorrHist.halve();
        materialCorrHist.halve();
        contCorrHist1.halve();
        contCorrHist2.halve();
        threatHist.halve();
    } else {
        // On any read failure, leave both tables untouched.
        pawnCorrHist.clear();
        materialCorrHist.clear();
    }
    return ok;
}

// 2026-05-17 audit #15/#1: factored quiet-move history helper. Mirrors
// SF18 src/search.cpp::update_quiet_histories (line 1893) sans the
// LowPlyHistory / PawnHistory branches that Hypersion has tombstoned.
void Worker::update_quiet_history(const Position& pos, Move m, int bonus,
                                   Piece prevPiece1, Move prevMove1,
                                   Piece prevPiece2, Move prevMove2) {
    mainHist.update(pos.side_to_move(), m, bonus);
    Piece moving = pos.piece_on(m.from_sq());
    if (prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none())
        contHist[0]->update(prevPiece1, prevMove1.to_sq(), moving, m.to_sq(), bonus);
    if (prevPiece2 != NO_PIECE && prevMove2 != Move::null() && prevMove2 != Move::none())
        contHist[1]->update(prevPiece2, prevMove2.to_sq(), moving, m.to_sq(), bonus / 2);
}

void Worker::prepare(const Position& srcPos, const SearchLimits& lim, ThreadPool* p,
                     int tid, bool main) {
    stop();
    wait_for_finish();

    pool     = p;
    threadId = tid;
    isMain   = main;

    // Build rootStates[] with the FULL StateInfo history chain from srcPos.
    // Without this, Position::set(fen, &rootStates[0]) sets up the board but
    // discards history, so during search is_draw() can never see a 3-fold
    // repetition that's still pending from the actual game (e.g. perpetual
    // checks already 2x in history; the 3rd should be a draw, but search
    // sees a fresh position and evaluates the perpetual as winning).
    //
    // Layout: rootStates[0..histN-1] = chronological history (oldest first),
    //         rootStates[histN]      = current root state (filled by set()).
    // previous-pointers thread the chain forward.
    rootStatePly = 0;
    {
        // Walk srcPos backward to collect history (most recent first).
        const StateInfo* hist[MAX_GAME_PLIES];
        int histN = 0;
        for (const StateInfo* s = srcPos.state();
             s != nullptr && histN < MAX_GAME_PLIES;
             s = s->previous, ++histN)
            hist[histN] = s;

        // Reverse-iterate so we copy oldest -> most-recent. Skip the last
        // one (= srcPos's current state); we'll let set(fen, ...) populate
        // that slot fresh and then re-link its previous pointer.
        StateInfo* prev = nullptr;
        for (int i = histN - 1; i >= 1; --i) {
            std::memcpy(static_cast<void*>(&rootStates[rootStatePly]),
                        hist[i], sizeof(StateInfo));
            rootStates[rootStatePly].previous = prev;
            prev = &rootStates[rootStatePly];
            ++rootStatePly;
        }

        // Now set up the current root state from FEN.
        rootPos.set(srcPos.fen(), &rootStates[rootStatePly]);

        // Re-link the chain. Position::set() resets previous to nullptr; we
        // restore it to point at the most-recent history we just copied.
        // Also restore pliesFromNull (FEN doesn't carry it, so set() left it
        // at 0; without this, repetition's `end = min(rule50, pliesFromNull)`
        // is always 0 and the walk-back never runs).
        rootStates[rootStatePly].previous = prev;
        if (histN > 0)
            rootStates[rootStatePly].pliesFromNull = hist[0]->pliesFromNull;

        // Recompute repetition for the new root with the linked chain.
        rootPos.recompute_repetition();
        ++rootStatePly;
    }

    limits = lim;
    tm.init(limits, rootPos.side_to_move(), rootPos.game_ply());
    nodes.store(0);
    selDepth = 0;
    completedDepth = 0;
    stopFlag.store(false);
    // Note: Lynx-style 3/4 history gravity tested and regressed -35 ELO.
    // Hypersion's update_history already does Stockfish-style soft-cap
    // decay (entry += bonus - entry * |bonus| / HISTORY_MAX); adding 3/4
    // multiplicative on top double-decays. The ButterflyHistory::gravity()
    // helper stays in history.h in case a future tuning pass wants it.

    rootMoves.clear();
    for (Move m : MoveList<LEGAL>(rootPos)) {
        // `go searchmoves` filter: when present, only search the listed moves.
        if (!limits.searchMoves.empty()) {
            bool found = false;
            for (Move sm : limits.searchMoves)
                if (sm == m) { found = true; break; }
            if (!found) continue;
        }
        RootMove rm; rm.pv0 = m; rm.pv.moves[0] = m; rm.pv.length = 1;
        rootMoves.push_back(rm);
    }
}

void Worker::launch() {
    isRunning.store(true);
    th = std::thread([this] { iterative_deepen(rootPos); isRunning.store(false); });
}

void Worker::stop() { stopFlag.store(true); }
void Worker::wait_for_finish() { if (th.joinable()) th.join(); }

bool Worker::should_stop() {
    if (stopFlag.load(std::memory_order_relaxed)) return true;
    if (pool && pool->global_stop().load(std::memory_order_relaxed)) return true;
    if (limits.nodes && nodes.load() >= std::uint64_t(limits.nodes)) return true;
    // Pondering: search until ponderhit / stop, never on time.
    if (pool && pool->ponder_flag().load(std::memory_order_relaxed)) return false;
    // Only the main worker enforces the clock — helpers run until the main
    // thread explicitly tells the pool to stop, so they keep filling the TT.
    if (isMain && !limits.infinite && limits.depth == 0) {
        // NOTE: 2026-05-12 tried rate-limiting tm.elapsed() to once per
        // 2048-node window via `if ((nodes.load() & 2047) != 0) return false;`
        // (skips ~99.95 % of chrono calls; worst-case time-budget overrun
        // ~2.7 ms at 770 kNPS — well inside the 30 ms moveOverhead buffer).
        // Bench NPS over 15 runs: 719 k vs 726 k baseline = -1.0 % (noise).
        // No measurable win — modern Windows steady_clock fast path
        // (QPC/RDTSC) is ~30-50 ns rather than the assumed 200 ns, and
        // the compiler likely already optimizes the branch-predicted
        // common case well. Reverted to keep code simple.
        TimePoint el = tm.elapsed();
        if (el >= tm.maximum()) return true;
        // Soft preemption: once we've spent > 3 × optimum we abandon the current
        // iteration — better to commit to the previous depth's result than burn
        // the rest of the clock on something we won't finish.
        if (el >= tm.optimum() * 3 && completedDepth >= 4) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------
void ThreadPool::set_size(int n) {
    stop_all();
    wait_all();
    workers.clear();
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) workers.emplace_back(std::make_unique<Worker>());
}

void ThreadPool::start(const Position& pos, const SearchLimits& lim) {
    stop_all();
    wait_all();
    if (workers.empty()) workers.emplace_back(std::make_unique<Worker>());

    stopAll.store(false);
    pondering.store(lim.ponder);    // search untimed until ponderhit / stop
    TT.new_search();

    // Helpers prepared first; the main worker is launched LAST so it sees the
    // freshly-launched helpers when it makes its first time-management call.
    for (size_t i = 0; i < workers.size(); ++i)
        workers[i]->prepare(pos, lim, this, int(i), /*isMain=*/i == 0);
    for (size_t i = 1; i < workers.size(); ++i) workers[i]->launch();
    workers[0]->launch();
}

void ThreadPool::stop_all() {
    stopAll.store(true);
    for (auto& w : workers) w->stop();
}

void ThreadPool::wait_all() {
    for (auto& w : workers) w->wait_for_finish();
}

void ThreadPool::clear_all() {
    for (auto& w : workers) w->clear();
    TT.clear();
}

bool ThreadPool::save_corr_hist(const std::string& path) const {
    if (workers.empty()) return false;
    return workers[0]->save_corr_hist(path);
}
bool ThreadPool::load_corr_hist(const std::string& path) {
    if (workers.empty()) return false;
    // Load into worker 0, then copy its tables into the other workers so
    // all threads start a game with the same learned baseline. (Workers
    // diverge naturally during search.)
    if (!workers[0]->load_corr_hist(path)) return false;
    for (size_t i = 1; i < workers.size(); ++i) {
        // Public copy: each Worker exposes its corr-history tables for
        // bulk-copy. We could write methods, but raw memcpy through the
        // friend-y access is just as safe given they're plain arrays.
        // Instead, re-load the file per worker (cheap, ~256 KB I/O each):
        workers[i]->load_corr_hist(path);
    }
    return true;
}

void ThreadPool::decay_all() {
    // Soft state reset for `ucinewgame`. Halve persistent histories instead
    // of zeroing them (keeps useful cross-game signal warm) and drop killers.
    // The TT keeps its data — TTEntry already has an age field that rotates
    // generations, so stale entries naturally lose priority.
    for (auto& w : workers) w->decay_for_new_game();
    TT.new_search();
}

void ThreadPool::reset_clock_start() {
    for (auto& w : workers) w->reset_clock();
}

std::uint64_t ThreadPool::total_nodes() const {
    std::uint64_t n = 0;
    for (auto& w : workers) n += w->nodes_searched();
    return n;
}

// ---------------------------------------------------------------------------
// Iterative deepening + aspiration windows
// ---------------------------------------------------------------------------
void Worker::iterative_deepen(Position& pos) {
    if (rootMoves.empty()) {
        if (isMain)
            std::cout << "info depth 0 score " << (pos.checkers() ? "mate 0" : "cp 0") << '\n'
                      << "bestmove 0000" << std::endl;
        return;
    }

    // ---- Root Syzygy probe ----
    // SF18-style per-move ranking via probe_root_dtz: Fathom assigns each
    // legal root move a tbRank where 1000 = "winning AND under no 50-move-
    // rule risk" and lower ranks come from `1000 - (dtz + rule50)` when
    // we're near the 50-move cliff. Above the cliff (dtz + rule50 > 99)
    // ranks separate sharply: a 2-ply-faster move can be the difference
    // between "ship in time" and "drawn by rule50".
    //
    // The legacy probe_root() bailed when rule50_count > 0, leaving the
    // engine to king-shuffle through TB-winning K+P / K+R+P endgames at
    // low time (user PGN game 2, drawn by 100-move adjudication despite a
    // winning advantage). The new path fires regardless of rule50.
    //
    // Measured improvement (testing/test_endgame_conversion.py at TC
    // wtime=2000 btime=2000 winc=0, 6 positions in 3-4-5 TB with rule50
    // already 40-85):
    //   PRE-DTZ (prevScore fix only):  2/6 converted to mate in 60 plies
    //   POST-DTZ (this change):        4/6 converted to mate in 60 plies
    // Both binaries: 0/6 TB-losing bestmoves. The fix converts where
    // PRE-DTZ shuffles into a 50-move draw. Codex audit 2026-05-12.
    //
    // No SPRT was run because SPRT openings are middlegame-heavy and
    // sample this regime sparsely. The targeted endgame test IS the
    // benchmark for this change.
    if (isMain && Syzygy::is_loaded()) {
        std::vector<Syzygy::RootMoveEntry> tbMoves;
        if (Syzygy::probe_root_dtz(pos, tbMoves) && !tbMoves.empty()) {
            int bestRank = INT_MIN;
            Move bestMv  = Move::none();
            for (auto& entry : tbMoves) {
                if (entry.rank > bestRank) {
                    bestRank = entry.rank;
                    bestMv   = entry.move;
                }
                // Match Fathom move -> rootMove entry, propagate rank+score
                // so the RootMove::operator< (tbRank desc, score desc) puts
                // the TB-best move at index 0 of rootMoves.
                for (auto& rm : rootMoves) {
                    if (rm.pv0 == entry.move) {
                        rm.tbRank = entry.rank;
                        rm.score  = entry.score;
                        break;
                    }
                }
            }
            std::stable_sort(rootMoves.begin(), rootMoves.end());
            std::cout << "info string syzygy: " << move_uci(bestMv)
                      << " (rank=" << bestRank
                      << ", rule50=" << pos.rule50_count() << ")"
                      << std::endl;
        }
    }

    // Stack array — sized to MAX_PLY+10 so search can safely peek ss-2 / ss+1.
    Stack stackArr[MAX_PLY + 10] = {};
    Stack* ss = stackArr + 4;   // leave 4 slots of headroom for ss-1, ss-2 references
    for (int i = -4; i <= MAX_PLY + 4; ++i) (ss + i)->ply = i;

    // ---- Skill-level / Elo limiter ----
    // Map UCI_LimitStrength + UCI_Elo to an effective skill level (0..20),
    // then cap depth and add move-selection noise at the end.
    //
    // CALIBRATION HISTORY: the previous mapping
    //   effSkill = (e - 500) * 20 / 2700;   depthCap = 1 + effSkill * 2;
    // was found to produce non-monotonic strength because depth 3-9 already
    // gives ~1800-2200 ELO with NNUE eval, regardless of move noise. The
    // noise spread `(20-skill)*60` was so large that all effSkill<20 plays
    // played near-randomly (best-move probability ~5%), making the configured
    // ELO essentially independent of skill in the 700-1500 range.
    //
    // NEW MAPPING: drives the strength via TWO levers:
    //   1. depthCap: shallower for low ELO (1 ply at ELO<700, ~ skill plies).
    //   2. nodesCap: hard node-count cap; severely weakens at very low ELO.
    //   3. moveNoise: probability of picking a non-best move from top-K.
    //
    // The mapping below is calibrated against the test_elo_scaling.py harness.
    // Bucket 0..28 corresponds to UCI_Elo 500..3300 in 100-ELO steps:
    //   bucket = (uciElo - 500) / 100,  clamped [0, 28].
    // (29-entry table covers the full lichess/chess.com bot range, then
    //  caps at 10 000 nodes / depth 10 / 5 % blunder for ELO >= 2500.
    //  No level plays at 0 % blunder — there's always some chance of a
    //  human-style mistake, matching what chess.com / Maia / Lichess bots
    //  do for natural feel.)
    int effSkill = limits.skillLevel;
    if (limits.limitStrength) {
        int e = std::clamp(limits.uciElo, 500, 3300);
        effSkill = (e - 500) / 100;
    }
    // skillLevel knob covers 0..20 (legacy SF range). Map skillLevel 20
    // to the highest table bucket (28, ELO 3300-equivalent) so the
    // limiter still applies when the user dials in a non-default skill.
    if (!limits.limitStrength && limits.skillLevel < 20) {
        effSkill = limits.skillLevel * 28 / 20;   // 0..20 -> 0..28
    }
    effSkill = std::clamp(effSkill, 0, 28);
    // Strength weakening via combined node cap + blunder rate.
    // CALIBRATED against Maia chess bots (lichess-trained at human Elos):
    //   - Hypersion full-strength bench plays ~2660 Elo
    //   - Each TC=30+0.3 move ~ 100k-200k nodes at full strength
    //   - Maia tests: at the previous calibration, Hyp@1100 lost 0-30 to
    //     Maia 1100 (under target by ~400 Elo); Hyp@1500 lost 23-7 to
    //     Maia 1500 (under target by ~250 Elo); Hyp@1900 was correct.
    //
    // FIX: drastically lower node caps so the engine actually plays at
    // the target depth/nodes. The blunder rate adds variance on top.
    //
    //  skill | UCI_Elo | blunder% |  node cap   (target real Elo)
    //   0    |   500   |   60%    |       30    (~ 600)
    //   1    |   635   |   45%    |       80
    //   2    |   770   |   30%    |      200
    //   3    |   905   |   20%    |      500
    //   4    |  1040   |   12%    |     1200    (~1100 vs Maia)
    //   5    |  1175   |    8%    |     2500
    //   6    |  1310   |    5%    |     5000
    //   7    |  1445   |    3%    |    10000    (~1500 vs Maia)
    //   8    |  1580   |    2%    |    25000
    //   9    |  1715   |    1%    |    60000
    //  10    |  1850   |    0%    |   150000    (~1900 vs Maia)
    //  11    |  1985   |    0%    |   400000
    //  12    |  2120   |    0%    |  1000000
    //  13+   |  2255+  |    0%    |  unlimited
    //  17-20 |  ~3200  |    0%    |  unlimited (full strength)
    // CALIBRATION ITERATION 3 (vs Maia 1100/1500/1900):
    // v2 was -380/-266/-200 ELO weak. Bumped node caps 3x and reduced
    // blunder rates by 30% to compensate.
    //
    //   skill | UCI_Elo | blunder% | nodes
    // v3.0+ casual-play strength table (29 entries, ELO 500..3300 in
    // 100-ELO buckets). Direct ELO -> {nodes, max_depth, blunder_%}
    // lookup. Hard caps: nodes <= 10 000, depth <= 10, blunder >= 5 %.
    // For ELO 2200 and under, depth <= 6 (per user spec — casual /
    // intermediate players shouldn't face deep tactical search).
    //
    // No level plays at 0 % blunder — even 3300-rated bots play one
    // human-style mistake every ~20 moves, matching chess.com / lichess
    // bot expectations. From 2500 up the table flatlines on nodes
    // (~10k) and depth (10); strength differentiation in that range is
    // pure blunder-frequency.
    //
    //   ELO  | Nodes | Depth | Blunder %
    //   500  |   100 |   2   |   25
    //   600  |   150 |   2   |   22
    //   700  |   200 |   2   |   20
    //   800  |   225 |   2   |   18
    //   900  |   250 |   2   |   16
    //  1000  |   300 |   3   |   15
    //  1100  |   350 |   3   |   14
    //  1200  |   400 |   4   |   13
    //  1300  |   500 |   4   |   12
    //  1400  |   650 |   5   |   11
    //  1500  |   800 |   5   |   10
    //  1600  |  1000 |   6   |    9
    //  1700  |  1300 |   6   |    9
    //  1800  |  1600 |   6   |    8   (depth-6 cap from 1800 up)
    //  1900  |  2000 |   6   |    8
    //  2000  |  2500 |   6   |    7
    //  2100  |  3000 |   6   |    7
    //  2200  |  3500 |   6   |    6   (last bucket at depth 6 max)
    //  2300  |  4500 |   7   |    6
    //  2400  |  6000 |   8   |    6
    //  2500  |  8000 |   9   |    5
    //  2600  |  8500 |  10   |    5
    //  2700  |  9000 |  10   |    5
    //  2800  |  9500 |  10   |    5
    //  2900  | 10000 |  10   |    5   (nodes flatline at 10k)
    //  3000  | 10000 |  10   |    5
    //  3100  | 10000 |  10   |    5
    //  3200  | 10000 |  10   |    5
    //  3300  | 10000 |  10   |    5   (ceiling — even 3300-rated stays here)
    //
    // Full strength (no caps) only when skillLevel=20 AND
    // limitStrength=false. Any other configuration applies the table.
    static constexpr int ELO_BLUNDER_PCT[29] = {
        25, 22, 20, 18, 16, 15, 14, 13, 12, 11,   // 500..1400
        10,  9,  9,  8,  8,  7,  7,  6,  6,  6,   // 1500..2400
         5,  5,  5,  5,  5,  5,  5,  5,  5         // 2500..3300
    };
    static constexpr std::uint64_t ELO_NODES[29] = {
          100ULL,   150ULL,   200ULL,   225ULL,   250ULL,   // 500..900
          300ULL,   350ULL,   400ULL,   500ULL,   650ULL,   // 1000..1400
          800ULL,  1000ULL,  1300ULL,  1600ULL,  2000ULL,   // 1500..1900
         2500ULL,  3000ULL,  3500ULL,  4500ULL,  6000ULL,   // 2000..2400
         8000ULL,  8500ULL,  9000ULL,  9500ULL, 10000ULL,   // 2500..2900
        10000ULL, 10000ULL, 10000ULL, 10000ULL              // 3000..3300
    };
    static constexpr int ELO_DEPTH_CAP[29] = {
         2,  2,  2,  2,  2,  3,  3,  4,  4,  5,   // 500..1400
         5,  6,  6,  6,  6,  6,  6,  6,  7,  8,   // 1500..2400 (depth-6 ceiling for <=2200)
         9, 10, 10, 10, 10, 10, 10, 10, 10        // 2500..3300
    };
    // Apply the ELO_TABLE caps in two cases:
    //   (a) UCI_LimitStrength=true  -> effSkill came from uciElo, always cap
    //   (b) skillLevel < 20         -> user explicitly chose a weaker level
    // skillLevel=20 with limitStrength=false = full strength (no caps).
    const bool applyEloCaps = limits.limitStrength || (limits.skillLevel < 20);
    int           skillBlunderPct = applyEloCaps ? ELO_BLUNDER_PCT[effSkill] : 0;
    int           skillDepthCap   = applyEloCaps ? ELO_DEPTH_CAP  [effSkill] : MAX_PLY;
    std::uint64_t skillNodeCap    = applyEloCaps ? ELO_NODES      [effSkill] : 0ULL;

    // Endgame conversion override (added 2026-05-10 after observing
    // lichess game H05vgtVr where Hypersion @ ELO ~1500 / 800-node-cap
    // failed to convert K+R vs K and drew by 50-move rule).
    //
    // When the position is a clear winning endgame (few pieces left AND
    // we have a meaningful material advantage), the strength cap is too
    // tight to find mate-in-N (N≥10 plies for K+R+K). Lift the node cap
    // and depth cap so the engine can actually convert. This preserves
    // strength-cap accuracy across the bulk of the game (where opponent
    // is still defending normally) but lets the engine close out won
    // endgames competently.
    //
    // Trigger conditions (all must hold):
    //   - applyEloCaps is true (we'd otherwise be limiting)
    //   - total piece count <= 10 (endgame; covers K+R+K through K+Q+P+K
    //     and similar)
    //   - non-pawn material balance >= ~ROOK value on our side (i.e.,
    //     we're up at least a rook in non-pawn material)
    if (applyEloCaps) {
        int totalPieces = popcount(rootPos.pieces());
        Color me = rootPos.side_to_move();
        Value myNPM = rootPos.non_pawn_material(me);
        Value oppNPM = rootPos.non_pawn_material(~me);
        bool clearlyWinningEndgame = (totalPieces <= 10) &&
                                     (int(myNPM) - int(oppNPM) >= int(Eval::PieceValueMG[ROOK]));
        if (clearlyWinningEndgame) {
            // FULLY DISABLE strength limiting in clearly-winning endgames.
            //
            // Rationale: Stockfish at UCI_Elo=1500 converts 8/12 K+R+K /
            // K+Q+K positions (test mate_sf_baseline.py, 2026-05-11). It
            // does this because SF's strength model only modifies move
            // SELECTION at one specific depth (skill.pick_best at
            // depth=1+level via MultiPV) — never caps nodes/depth. The
            // engine still SEES the full search tree.
            //
            // Hypersion's strength model caps nodes hard (~800 nodes at
            // UCI_Elo=1500). At that budget the engine can't find a
            // 16-ply K+R+K mating plan. Even 16x node boost (12.8k) and
            // blunder=0 gave a 50-move-rule shuffle.
            //
            // The realistic behavior for a 1500-rated human who's up
            // a rook is: they convert reliably. Basic mate-in-N drills
            // are explicit chess-school knowledge. So in
            // clearlyWinningEndgame, we treat the engine as full
            // strength.
            skillNodeCap   = 0;          // 0 = no cap
            skillDepthCap  = MAX_PLY;    // effectively no cap
            skillBlunderPct = 0;
        }
    }

    if (limits.nodes > 0)
        skillNodeCap = (skillNodeCap == 0) ? std::uint64_t(limits.nodes)
                                           : std::min<std::uint64_t>(skillNodeCap, limits.nodes);
    // Apply skill-based node cap by overriding limits.nodes — should_stop()
    // already enforces this at every node.
    if (skillNodeCap > 0)
        limits.nodes = std::int64_t(skillNodeCap);

    int targetDepth = limits.depth > 0 ? limits.depth : MAX_PLY - 4;
    targetDepth     = std::min(targetDepth, skillDepthCap);
    int multiPv     = std::min<int>(std::max(1, limits.multiPv), int(rootMoves.size()));
    Value bestScore = -VALUE_INFINITE;
    Move  bestMove  = rootMoves[0].pv0;
    PVLine bestPV;

    Value prevScore       = -VALUE_INFINITE;
    bool  havePrevScore   = false;  // gates readers of prevScore — covers SMP-
                                    // helper case where threads skip early
                                    // depths and would otherwise compare
                                    // against the sentinel.
    Move  prevBestMove    = Move::none();
    int   bestMoveChanges = 0;     // how many recent iterations changed bestmove
    int   stableIters     = 0;     // consecutive iterations with same bestmove + small score change
    // SF18-style per-iteration bestMoveChanges tracking (src/search.cpp:480, 503).
    // SF accumulates intra-iteration bestmove changes across PV lines, then
    // decays /=2 between iterations so the time-scale factor reflects RECENT
    // instability rather than cumulative-since-iter-1. Hypersion's previous
    // bestMoveChanges was cumulative-only and exploded the SF time factor
    // (-111 ELO at 200g). Per-iter version is the correct port.
    double totBestMoveChanges = 0.0;
    int    iterBestMoveChanges = 0;
    int    moveCountAtLastBest = 0;

    // SMP diversity (Stockfish-style depth skipping for helpers). Helpers
    // skip iterations of the iterative-deepening loop on a per-thread
    // schedule so each helper explores the tree at a different pacing.
    // This avoids TT contention (everyone hitting the same slots at the
    // same depth) and pushes each helper toward different parts of the
    // PV tree, so the shared TT picks up better information than a single
    // thread alone would gather.
    //
    // Formula (SF18): skip iteration if `(depth + skipPhase) / skipSize`
    // is odd. Main thread (threadId == 0) never skips.
    static constexpr int SKIP_SIZE [20] = {1,2,2,4,4,4,8,8,8,8,16,16,16,16,16,16,32,32,32,32};
    static constexpr int SKIP_PHASE[20] = {0,0,1,0,1,2,0,1,2,3,0,1,2,3,4,5,0,1,2,3};

    for (int d = 1; d <= targetDepth; ++d) {
        if (!isMain && threadId > 0) {
            int t = (threadId - 1) % 20;
            if (((d + SKIP_PHASE[t]) / SKIP_SIZE[t]) % 2 != 0)
                continue;
        }
        selDepth = 0;
        iterBestMoveChanges = 0;   // reset per-iter counter; accumulated on
                                   // each non-first root move that becomes best

        // ---- MultiPV loop: search top-N root moves separately ----
        // pvIdx is the rank we're searching for. After each pvIdx, we've fixed
        // the PV-th best move at rootMoves[pvIdx]. Lower indices already done.
        for (int pvIdx = 0; pvIdx < multiPv; ++pvIdx) {
            Value windowAlpha = -VALUE_INFINITE, windowBeta = VALUE_INFINITE;
            int   delta = ASPIRATION_DELTA0;
            // Tombstone (v13, 2026-05-15): tried widening initial aspiration
            // window 4x for first-own-search (ownSearchIndex == 1) on the
            // theory that TT-cold prevScore at d=3 is unreliable. SPRT
            // result at TC 5+0.05 conc=6:
            //     Stage 1  30g triage : +58.5 +/- 119.5 ELO (15W-10L-5D)
            //     Stage 2 200g        : -10.4 +/- 38.3 ELO (60W-66L-74D)
            //   REJECTED.
            // Same failure mode as v8 H1: candidate-as-Black scored 39 %
            // vs 50 % parity (22W-43L-34D Black, 36W-22L-40D White). Both
            // attempts to "give first own search more resources" (time in
            // v8 H1, wider window here) hurt Black-side play. Hypothesis:
            // at fast TC the deeper-or-more-reliable first eval reveals
            // Black's structural disadvantage too clearly, causing the
            // engine to commit to passive defense. Aspiration is already
            // tuned at delta=50 for ALL ply>=4 iterations; widening only
            // on first move loses the narrowing benefit on that move
            // without compensating gain.
            // Future contributors: don't re-test 4x. Possible variants:
            //   - TIGHTER window on first move (delta=25, opposite direction).
            //   - Wider window on later moves (5-15) where real blunders
            //     cluster per testing/BLUNDER_ANALYSIS.md.
            //   - Color-asymmetric tuning (different delta for White vs Black).
            if (d >= 4 && std::abs(rootMoves[pvIdx].prevScore) < VALUE_MATE_IN_MAX_PLY) {
                windowAlpha = std::max<int>(rootMoves[pvIdx].prevScore - delta, -VALUE_INFINITE);
                windowBeta  = std::min<int>(rootMoves[pvIdx].prevScore + delta,  VALUE_INFINITE);
            }

            while (true) {
                Value alpha = windowAlpha;
                Value bestThisIter = -VALUE_INFINITE;
                PVLine iterPV;
                int    moveIndex = 0;

                // Only search rootMoves[pvIdx..] — earlier entries are already fixed.
                for (size_t i = pvIdx; i < rootMoves.size(); ++i) {
                    RootMove& rm = rootMoves[i];
                    StateInfo st;
                    PVLine    childPv;
                    ss->currentMove = rm.pv0;
                    ss->movedPiece  = pos.piece_on(rm.pv0.from_sq());
                    pos.do_move(rm.pv0, st);
                    TT.prefetch(pos.key());
                    pawnCorrHist.prefetch(pos.side_to_move(), pos.pawn_key());
                    // Track per-root-move node effort for SF-style bestMoveEffort
                    // time scaling. Capture the node count snapshot before this
                    // root move's subtree begins.
                    std::uint64_t nodesBeforeMove = pool ? pool->total_nodes()
                                                         : nodes.load(std::memory_order_relaxed);
                    Value v;
                    if (moveIndex == 0) {
                        v = -search(pos, ss + 1, -windowBeta, -alpha, d - 1, childPv, true, false);
                    } else {
                        v = -search(pos, ss + 1, -alpha - 1, -alpha, d - 1, childPv, false, true);
                        if (!stopFlag.load() && v > alpha && v < windowBeta)
                            v = -search(pos, ss + 1, -windowBeta, -alpha, d - 1, childPv, true, false);
                    }
                    pos.undo_move(rm.pv0);
                    // Accumulate effort: nodes consumed by this root move's subtree.
                    std::uint64_t nodesAfterMove = pool ? pool->total_nodes()
                                                        : nodes.load(std::memory_order_relaxed);
                    if (nodesAfterMove > nodesBeforeMove)
                        rm.effort += nodesAfterMove - nodesBeforeMove;

                    if (should_stop()) break;

                    // 2026-05-17 CRITICAL FIX (replaces an earlier incorrect
                    // attempt): the searched value `v` MUST be stored in
                    // rm.score so the NNUE-eval-based tiebreak among
                    // DTZ-tied root moves still works. Previously this
                    // code overwrote `v` with VALUE_TB_WIN - 100 for all
                    // tb-decisive moves, collapsing the tiebreak —
                    // bishop-shuffles and king-moves ended up with the
                    // same rm.score and stable_sort fell back to movegen
                    // order. User-reported KBBK FEN
                    // `8/8/3k4/8/8/8/4K3/B6B w` shuffled Ba1↔Bb2
                    // indefinitely (50-move draw) because Fathom's
                    // rank=1000 was ignored once the score collapsed.
                    //
                    // The display-side TB-WIN reporting (to match SF's
                    // cp 20000 at root) is now handled separately in
                    // print_info, which uses the rootMove's tbRank/score
                    // pair to derive a TB-decisive display value without
                    // touching the search-side ordering data.
                    rm.score = v;
                    rm.selDepth = std::max(rm.selDepth, selDepth);
                    if (v > bestThisIter) {
                        bestThisIter = v;
                        update_pv(iterPV, rm.pv0, childPv);
                        rm.pv = iterPV;
                        if (v > alpha) alpha = v;
                        // SF18-style intra-iter bestmove change tracking.
                        // Increment whenever a non-first move (moveIndex > 0)
                        // raises the iter's best — only on first PV line so
                        // multipv runs don't double-count. Source: SF18
                        // src/search.cpp:1342-1346.
                        if (moveIndex > 0 && pvIdx == 0)
                            ++iterBestMoveChanges;
                    }
                    ++moveIndex;
                }

                if (should_stop()) goto done;

                if (bestThisIter <= windowAlpha && windowAlpha != -VALUE_INFINITE) {
                    // Fail-low: report partial info with `upperbound`.
                    if (isMain && pvIdx == 0)
                        print_info(d, selDepth, bestThisIter,
                                   pool ? pool->total_nodes() : nodes.load(),
                                   tm.elapsed(), iterPV, 0, multiPv, /*flag=*/1);
                    // 2026-05-17 SF18 fail-low: new beta = OLD alpha (not
                    // midpoint). The re-search then has window
                    // [bestThisIter - delta, oldAlpha] which is what SF18
                    // does at src/search.cpp:402-403. Midpoint produced a
                    // wider window than SF, wasting re-search nodes.
                    windowBeta  = windowAlpha;
                    windowAlpha = std::max<int>(bestThisIter - delta, -VALUE_INFINITE);
                    delta += delta / 4 + ASP_GROWTH_ADD;   // A7: pre-tunable was hardcoded 5
                } else if (bestThisIter >= windowBeta && windowBeta != VALUE_INFINITE) {
                    // Fail-high: report partial info with `lowerbound`.
                    if (isMain && pvIdx == 0)
                        print_info(d, selDepth, bestThisIter,
                                   pool ? pool->total_nodes() : nodes.load(),
                                   tm.elapsed(), iterPV, 0, multiPv, /*flag=*/2);
                    // 2026-05-17 SF18 fail-high: also RAISE alpha so the
                    // re-search doesn't waste effort exploring scores far
                    // below the new beta. SF18 src/search.cpp:410.
                    windowAlpha = std::max<int>(windowBeta - delta, windowAlpha);
                    windowBeta = std::min<int>(bestThisIter + delta, VALUE_INFINITE);
                    delta += delta / 4 + ASP_GROWTH_ADD;   // A7: pre-tunable was hardcoded 5
                } else {
                    if (pvIdx == 0) { bestScore = bestThisIter; bestPV = iterPV; }
                    break;
                }
                if (delta >= ASP_FULL_WINDOW_TH) { windowAlpha = -VALUE_INFINITE; windowBeta = VALUE_INFINITE; }
            }

            // Bring the best of the searched range to position pvIdx.
            std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());
        }

        // Remember scores for the next iteration's aspiration window.
        for (auto& rm : rootMoves) rm.prevScore = rm.score;

        bestMove       = rootMoves[0].pv0;
        // NOTE: prevScore is intentionally NOT updated here. It must hold
        // the previous COMPLETED iteration's bestScore for the smallSwing
        // (line ~1242) and fallingEval (line ~1298) comparisons below.
        //
        // Bug found 2026-05-12 by Codex audit, confirmed by source
        // inspection: pre-fix this assignment lived here, making
        // `bestScore - prevScore` always 0 -> smallSwing permanently true
        // (stableIters incremented unconditionally -> 0.5x time scale too
        // aggressively), fallingEval permanently 1.0 (eval-drop time bump
        // never fired). SPSA-tuned STABILITY_SWING_TH=61 and
        // FALLING_EVAL_DIV=1058 were tuned over dead code.
        //
        // Update now at the bottom of the iteration body after both
        // smallSwing and fallingEval have read the old value. A
        // `havePrevScore` boolean (instead of `d > 1`) gates readers, so
        // SMP-helper threads that skip early depths (line ~1099) don't
        // compare bestScore against the sentinel -VALUE_INFINITE.
        //
        // SPRT measurement (2026-05-12, conc=6, TC 5+0.05):
        //   POST-FIX vs PRE-FIX (200g):                 -3.5 +/- 35.5 ELO
        //   ABLATION (no fallingEval) vs PRE-FIX (200g): -6.9 +/- 38.6 ELO
        // Statistically indistinguishable from no-op. Code is correct now;
        // both formerly-dead heuristics are firing within their tuned
        // ranges, but they nearly cancel at bullet TC (smallSwing
        // tightening slightly slows play, fallingEval bump slightly speeds
        // conversion). Path to recover ELO: SPSA retune
        // STABILITY_SWING_TH + FALLING_EVAL_DIV over the now-active code,
        // and validate at LTC where Codex prior was "+5 to +15 ELO".
        completedDepth = d;

        // Only the main worker emits UCI info. Helpers run silently to keep
        // the output stream readable and to fill the shared TT.
        if (isMain) {
            std::uint64_t totN = pool ? pool->total_nodes() : nodes.load();
            TimePoint     ms   = tm.elapsed();
            for (int pvIdx = 0; pvIdx < multiPv && pvIdx < int(rootMoves.size()); ++pvIdx)
            {
                // 2026-05-17 display-only TB-WIN override: when the root
                // probe identified this move as TB-decisive winning, show
                // a TB-WIN-range score to the GUI (matching SF18's
                // cp 20000 at root for tb-decisive positions). The
                // searched value `score` is kept intact for ordering
                // purposes; only the displayed score is upgraded.
                Value displayScore = rootMoves[pvIdx].score;
                if (rootMoves[pvIdx].tbRank > 0
                    && std::abs(displayScore) < VALUE_TB_WIN_IN_MAX_PLY) {
                    displayScore = Value(VALUE_TB_WIN - 100);
                }
                print_info(d, selDepth, displayScore, totN, ms, rootMoves[pvIdx].pv,
                           pvIdx, multiPv);
            }
        }

        if (should_stop()) break;
        if (std::abs(bestScore) >= VALUE_MATE_IN_MAX_PLY) break;

        // Track score / bestmove stability for time scaling.
        // Gate on havePrevScore (not d > 1): SMP-helper threads may skip
        // early depths via the distribution at line ~1091 and thus first
        // arrive here at d > 1 with prevScore still at the sentinel
        // -VALUE_INFINITE. Codex audit P3, 2026-05-12.
        if (havePrevScore) {
            bool sameMove   = (bestMove == prevBestMove);
            bool smallSwing = std::abs(int(bestScore - prevScore)) < STABILITY_SWING_TH;
            if (sameMove && smallSwing) ++stableIters;
            else                        stableIters = 0;
            if (!sameMove)              ++bestMoveChanges;
        }
        prevBestMove = bestMove;

        // SF18 totBestMoveChanges decay (src/search.cpp:327, 480-481):
        // halve the running total then add this iter's changes. The /2
        // decay means the time factor reflects RECENT instability rather
        // than cumulative-since-start.
        totBestMoveChanges = totBestMoveChanges / 2.0 + iterBestMoveChanges;
        (void)moveCountAtLastBest;  // reserved for future SF-port use

        // Soft-stop: scale optimum budget by stability. Stable for 4+ iterations
        // → take only ~50 % of optimum; volatile (recent move changes) → 1.4 ×.
        if (isMain && !limits.infinite && limits.depth == 0) {
            double scale = 1.0;
            // A11: pre-tunable were hardcoded 0.5 / 0.75
            if (stableIters >= 4) scale = STABLE_HIGH_SCALE / 100.0;
            else if (stableIters >= 2) scale = STABLE_LOW_SCALE / 100.0;

            // Compute *current* remaining clock (limits.time[us] is at
            // search-start; tm.elapsed() is what we've already burned this
            // move). Drives the "very-low-clock" gates below.
            Color usCol = rootPos.side_to_move();
            int64_t remMs = (limits.time[usCol] > 0)
                ? int64_t(limits.time[usCol]) - int64_t(tm.elapsed())
                : INT64_MAX;
            const bool veryLowClock = (remMs >= 0 && remMs < 2000);   // < 2 s

            // SF18-style "spend more on volatile bestmove" — but ONLY when
            // we have enough clock to chase the answer. blunder_histogram.py
            // (testing/, 2026-05-08) measured 94 % of Hypersion's
            // self-detected blunders happen with < 2 s remaining; the
            // volatility bump amplifies the leak by burning clock on
            // volatile positions whose tactics we can't resolve at the
            // depth we'd reach anyway. Below 2 s we keep scale flat.
            if (bestMoveChanges >= 3 && d <= 12 && !veryLowClock) scale *= 1.4;

            // Falling-eval factor (Stockfish 18 src/search.cpp). When the
            // score has DROPPED from the previous iteration, the position
            // is harder than we thought — spend more time. When it has
            // RISEN, we're finding the win — save time. This directly
            // addresses the user-reported bullet bug: with a passed pawn
            // and rook, eval starts high but if opponent finds a check
            // that drops it temporarily, fallingEval bumps up and we
            // search deeper to find the actual conversion.
            //
            // Clamped to [0.6, 1.7] — never less than 60 % of base time
            // (so we don't blitz away the win) nor more than 170 %
            // (so we don't flag).
            //
            // Scale: Hypersion's eval is at SF's 5x scale, so divide
            // the eval-drop-to-time factor by 5 vs SF's coefficient.
            if (havePrevScore) {
                int drop = int(prevScore - bestScore);   // > 0 when eval dropped
                // A10: pre-tunable was hardcoded /1000.0
                double fallingEval = std::clamp(1.0 + double(drop) / double(FALLING_EVAL_DIV),
                                                0.6, 1.7);
                scale *= fallingEval;
            }

            // Endgame time bonus. SF analysis on a 50-game match against
            // full Stockfish showed 70% of Hypersion's blunders happen in
            // endgame phases (early-end + deep-end), and 35% involve king
            // moves. In low-piece-count positions every move can be decisive,
            // and the stability heuristic above tends to cut us short on the
            // exact positions where we need precision. Boost time when the
            // total piece count is low. The bonus stacks on top of (or
            // mitigates) the stability cut, which is the right direction:
            // stable+endgame -> still spend reasonable time.
            int totalPieces = popcount(rootPos.pieces());
            // NOTE: tested rebalancing time multipliers post-cutoffCnt-ship
            // (which made endgame play reliable: 10.5/12 vs SF-2400 with no
            // >200cp endgame blunders). Reduced endgame bonuses (1.6/1.4/1.2
            // -> 1.4/1.25/1.1) and added middlegame boost (>=24 pieces: x1.15)
            // to redirect budget toward the remaining 100-200cp middlegame
            // tactical inaccuracies.
            // Trajectory:
            //   30g:  +11.6 +/- 101.6 ELO  (noise band)
            //   200g: +13.9 +/-  38.0 ELO  (above SHIP bar — looked promising)
            //   300g: -39.5 +/-  28.6 ELO  (REJECT — 200g was an opening-set
            //                              outlier; combined 500g gives ~-18 ELO)
            // The two SPRT runs use independent random opening subsets — the
            // 200g run drew openings that favoured the rebalance, the 300g run
            // drew unfavourable ones. Combined, the rebalance is mildly
            // negative.  Lesson: at 200g with CI ±35-40 a +14 result still
            // has substantial chance of being noise.  Future contributor
            // wanting to retry should run 500g+ in one shot, or pair the
            // rebalance with a measured-from-game-data piece-count profile
            // showing where Hypersion actually loses the most time.
            if (totalPieces <= 8)        scale *= TM_ENDGAME_BONUS_8  / 100.0;   // K+R+P-ish endgames
            else if (totalPieces <= 12)  scale *= TM_ENDGAME_BONUS_12 / 100.0;   // light endgame
            else if (totalPieces <= 16)  scale *= TM_ENDGAME_BONUS_16 / 100.0;   // late middlegame transition

            // Phase 5: easy-move detection. When the best root move is
            // clearly better than the 2nd-best AND has been stable, we don't
            // need to keep thinking. NNUE-aware thresholds (~/3 divisor puts
            // avg eval at ~220 cp). Uses `min` so this only ever saves time
            // — never extends past the existing scale.
            if (d >= 6 && rootMoves.size() >= 2 && stableIters >= 3) {
                int gap = int(rootMoves[0].score - rootMoves[1].score);
                // At low remaining clock (< 2 s), be more aggressive about
                // saving time on easy moves — we need the headroom for the
                // hard tactical positions in the endgame conversion. Same
                // motivation as the volatility-bump skip above.
                if (veryLowClock) {
                    if (gap >= 80)        scale = std::min(scale, 0.30);
                    else if (gap >= 40)   scale = std::min(scale, 0.50);
                    else if (gap >= 20)   scale = std::min(scale, 0.75);
                } else {
                    if (gap >= 150)       scale = std::min(scale, TM_EASY_GAP150 / 100.0);
                    else if (gap >= 80)   scale = std::min(scale, TM_EASY_GAP80  / 100.0);
                    else if (gap >= 40)   scale = std::min(scale, TM_EASY_GAP40  / 100.0);
                }
            }

            // Best-move effort scaling (Stockfish 18 master src/search.cpp).
            // When the search has concentrated >= ~93 % of its node budget
            // on the current best move, the engine is confident — drop time
            // by a hard 24 %. Below that threshold, no scaling. Simpler and
            // sharper than the old interpolation, matches SF master.
            //
            // Gated on d >= 6 so the early iterations (which naturally
            // have wildly varying effort) don't trigger early exits.
            if (d >= 6 && !rootMoves.empty()) {
                std::uint64_t totN = pool ? pool->total_nodes() : nodes.load();
                if (totN > 1) {
                    std::uint64_t bestEffort = rootMoves[0].effort;
                    int nodesEffort = int(bestEffort * 100000ULL / totN);
                    // A10: pre-tunable was hardcoded 93340 / 0.76
                    if (nodesEffort >= EFFORT_TH)
                        scale *= EFFORT_SCALE / 100.0;
                }
            }

            // NOTE: Phase 6.3 attempted SF18 best-move-instability time
            // factor with proper per-iteration tracking (totBestMoveChanges
            // with /2 decay per iter, per-iter `iterBestMoveChanges` fed
            // by intra-iter bestmove changes at root).  This was meant to
            // fix the previous tombstone (-111 ELO) which used a cumulative
            // counter. Tested two coefficient strengths:
            //   v1: SF's 1.02 + 2.14*tBMC/nThreads, clamp [0.8, 2.0]
            //       30g: -46.6, 200g: -27.9 ELO (reject)
            //   v2: 1.0 + 1.0*tBMC/nThreads, clamp [0.9, 1.5]
            //       30g: -107.5 ELO (worse — lower clamp doesn't help)
            // Both regress because at fast TC (5+0.05), any time-bonus on
            // unstable iterations pushes us toward flag-outs in the endgame.
            // The instability signal IS real, but the "spend more time"
            // response is wrong for Hypersion's bullet calibration.
            // Future contributor: try INVERSE — REDUCE time on stable
            // iters (instabilityFactor between [0.7, 1.0]) so the
            // benefit shows in shorter total time.  Or test only at
            // longer TC where flag-out isn't a concern.
            (void)totBestMoveChanges;
            (void)iterBestMoveChanges;

            // KNOWN-ISSUE (user-reported bullet bug): at very low remaining
            // time with a winning advantage (passed pawn / extra rook), the
            // engine plays "panic" moves that don't progress the conversion.
            // Two attempted fixes (timeman mtg compression, scramble-time
            // bonus) both regressed in 200-game A/Bs — they helped the
            // specific case but cost overall ELO via flag-outs in long
            // games. Root cause: at <1 s remaining there's genuinely not
            // enough time per move to find a 12+ ply conversion sequence,
            // and any time bonus pushes us past the safe maximum.
            // Mitigation that DOES help: ensure SyzygyPath is configured
            // (lichess-bot config-hypersion.yml has it). With Syzygy 3-4-5
            // loaded, K+R+P-vs-K and similar small endings play perfectly
            // from the tablebase regardless of remaining time.

            TimePoint optScaled = TimePoint(tm.optimum() * scale);
            // Hard cap — never let the endgame bonus push past tm.maximum().
            optScaled = std::min<TimePoint>(optScaled, tm.maximum());
            if (tm.elapsed() > optScaled) break;
        }

        // End-of-iteration: save this iter's bestScore so the NEXT iter can
        // compare against it via smallSwing / fallingEval. Pre-2026-05-12 this
        // happened at the top of the iteration, before the comparisons —
        // making them no-ops. See the long comment higher in this loop.
        prevScore     = bestScore;
        havePrevScore = true;
    }

done:
    if (isMain) {
        // ---- Skill-level move noise (blunder rate) ----
        // With probability skillBlunderPct/100, override the search's best
        // move with a random move drawn from the top half of root moves
        // (so the blunder is a "human-plausible" mistake, not a wild
        // rookie blunder). The test_elo_scaling.py harness calibrates the
        // BLUNDER_PCT values per UCI_Elo bucket.
        if (skillBlunderPct > 0 && rootMoves.size() > 1) {
            static thread_local std::mt19937 prng(std::random_device{}() ^ 0xA5A5A5A5u);
            int roll = std::uniform_int_distribution<int>(1, 100)(prng);
            if (roll <= skillBlunderPct) {
                std::stable_sort(rootMoves.begin(), rootMoves.end());
                // Blunder pool: top half of root moves, minimum 2.
                int blunderPool = std::max<int>(2, int(rootMoves.size()) / 2);
                blunderPool = std::min<int>(blunderPool, int(rootMoves.size()));
                int pick = std::uniform_int_distribution<int>(0, blunderPool - 1)(prng);
                bestMove = rootMoves[pick].pv0;
            }
        }
        // Main thread chose its bestmove; ask the pool to halt the helpers.
        if (pool) pool->global_stop().store(true);
        // Look up the move we'd ponder on (the second move of the PV).
        Move ponderMove = Move::none();
        for (const auto& rm : rootMoves) {
            if (rm.pv0 == bestMove && rm.pv.length >= 2) { ponderMove = rm.pv.moves[1]; break; }
        }
        // 2026-05-17 audit uci [25]: Chess960-aware UCI output. For
        // standard positions move_uci(m, pos) collapses to move_uci(m);
        // for Chess960 it emits king-takes-rook castling notation.
        std::cout << "bestmove " << move_uci(bestMove, rootPos);
        if (ponderMove != Move::none()) std::cout << " ponder " << move_uci(ponderMove, rootPos);
        std::cout << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Quiescence search
// ---------------------------------------------------------------------------
Value Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta, bool isPv) {
    nodes.fetch_add(1, std::memory_order_relaxed);
    int ply = ss->ply;
    if (ply > selDepth) selDepth = ply;
    if (should_stop()) return VALUE_ZERO;
    if (pos.is_draw(ply))
        return value_draw(nodes.load(std::memory_order_relaxed), limits.contempt);
    bool inCheck = pos.checkers();
    // 2026-05-17 qsearch finding #5: at MAX_PLY, SF18:1538 returns
    // VALUE_DRAW when in check (no further moves can be searched) and
    // evaluate(pos) otherwise. Hypersion previously returned the raw
    // eval unconditionally — meaningless during a check.
    if (ply >= MAX_PLY)   return inCheck ? VALUE_DRAW : Eval::evaluate(pos);

    // 2026-05-17 audit #4: same upcoming-repetition alpha-bump as in search().
    // SF18 search.cpp:1505. Doubly important in qsearch where standpat-only
    // paths would otherwise miss "I can force a repetition" draws.
    if (alpha < VALUE_DRAW && pos.upcoming_repetition(ply)) {
        alpha = value_draw(nodes.load(std::memory_order_relaxed), limits.contempt);
        if (alpha >= beta) return alpha;
    }


    ss->inCheck  = inCheck;

    bool ttHit = false;
    TTEntry* tte = TT.probe(pos.key(), ttHit);
    Value ttValue = ttHit ? TT.value_from_tt(tte->value(), ply, pos.rule50_count()) : VALUE_NONE;
    Move ttMove   = ttHit ? tte->move() : Move::none();

    if (!isPv && ttHit && tte->depth() >= 0) {
        if (tte->bound() == BOUND_EXACT
            || (tte->bound() == BOUND_LOWER && ttValue >= beta)
            || (tte->bound() == BOUND_UPPER && ttValue <= alpha))
            return ttValue;
    }

    Value bestValue;
    Value staticEval;
    if (inCheck) {
        bestValue = staticEval = -VALUE_INFINITE;
        ss->staticEval = staticEval;
    } else {
        Value raw = ttHit && tte->eval() != VALUE_NONE ? tte->eval() : Eval::evaluate(pos);
        // 2026-05-17 qsearch finding #12: apply correction history in qsearch
        // too. Previously the main search corrected the eval but qsearch's
        // stand-pat used the raw NNUE — so the same position reported
        // different eval depending on whether main search or qsearch
        // reached it first. SF18:1573-1579 wraps in `to_corrected_static_eval`.
        staticEval = pawnCorrHist.adjust(pos.side_to_move(), pos.pawn_key(), raw);
        bestValue  = staticEval;
        // 2026-05-17 qsearch finding #39: write ss->staticEval BEFORE the
        // stand-pat short-circuit so the value is set even on fail-high
        // return. Previously the assignment lived after the `if (>= beta)
        // return` and was skipped on stand-pat exit, leaving the child's
        // ss->staticEval as whatever the previous occupant left there.
        ss->staticEval = staticEval;
        // NOTE: tested SF18 src/search.cpp:1577-1579 qsearch ttValue stand-pat
        //   `if (ttHit && bound matches) bestValue = ttValue`
        // Trajectory:
        //   30g:  -11.6 +/- 107.2 ELO  (noise)
        //   100g: +10.4 +/-  52.6 ELO  (at ship bar)
        //   200g: -12.2 +/-  38.2 ELO  (REJECT, tombstone)
        // Noise band, trending negative.  Hypothesis: using a deeper
        // search's ttValue at qsearch means we stand-pat earlier and skip
        // captures that would have refuted that ttValue at qsearch's
        // local horizon — qsearch loses tactical accuracy.  In SF this is
        // compensated by tighter capture pruning gates we don't have.
        if (bestValue >= beta)  { return bestValue; }
        if (bestValue >  alpha) alpha = bestValue;
    }

    // 2026-05-17 audit qs #18: pass contHist to qsearch MovePicker so
    // evasion ordering can use the parent's continuation-history gradient.
    Move  qPrevMove1  = (ss - 1)->currentMove;
    Piece qPrevPiece1 = (ss - 1)->movedPiece;
    Move  qPrevMove2  = (ss - 2)->currentMove;
    Piece qPrevPiece2 = (ss - 2)->movedPiece;
    MovePicker mp(pos, ttMove, &mainHist, &captureHist, /*qDepth=*/0,
                  contHist[0].get(), qPrevMove1, qPrevPiece1,
                  contHist[1].get(), qPrevMove2, qPrevPiece2);
    Move bestMove = Move::none();

    // 2026-05-17 audit qs #23: MaxQsearchGain replaced by per-victim
    // PieceValueMG[victim] inside the futility check below. QSEARCH_CAP_GAIN
    // tunable kept as a fallback constant (still exposed via SPSA), but no
    // longer referenced in the per-move pruning.
    (void)QSEARCH_CAP_GAIN;
    Move m;
    while ((m = mp.next_move()) != Move::none()) {
        if (!pos.legal(m)) continue;

        // SEE pruning in qsearch: skip captures that lose material.
        // 2026-05-16 threshold widened to TB-loss range (was MATE_IN_MAX).
        // SF18 src/search.cpp:1632 uses `!is_loss(bestValue)` so qsearch
        // pruning is also gated against TB-loss scores, not just mate-loss.
        // 2026-05-17 audit qs #21: SF18 uses SEE threshold -80 cp (slightly
        // negative) rather than 0 — accepts small-loss captures because in
        // qsearch they may still lead to a recapture or tactical sequence
        // worth searching. At Hypersion's 5x eval scale, -80 cp = -400.
        // SHIPPED WITHOUT SPRT — magnitude pulled from SF reference; if
        // Hypersion regresses, revert to VALUE_ZERO.
        if (!inCheck && bestValue > VALUE_TB_LOSS_IN_MAX_PLY && !pos.see_ge(m, Value(-400)))
            continue;

        // Capture-futility in qsearch: even capturing a queen wouldn't lift our
        // score to alpha, so don't bother. (Skipped while in check — every
        // evasion must be considered.)
        // 2026-05-17 qsearch finding #20: SF18:1641-1657 exempts RECAPTURES
        // of the piece that just moved (`move.to_sq() == prevSq`) from
        // futility pruning. A recapture of just-captured material is a
        // forced trade, not a speculative grab — pruning it loses tactical
        // accuracy in the most common qsearch scenario.
        if (!inCheck && pos.capture(m) && bestValue > VALUE_TB_LOSS_IN_MAX_PLY) {
            Square prevSq = ((ss - 1)->currentMove != Move::none()
                          && (ss - 1)->currentMove != Move::null())
                            ? (ss - 1)->currentMove.to_sq() : SQ_NONE;
            if (m.to_sq() != prevSq) {
                // 2026-05-17 audit qs #23: per-victim futility (SF18
                // src/search.cpp:1648-1656). Previously Hypersion used a
                // single MaxQsearchGain ≈ QUEEN + slack, so capturing a pawn
                // and a queen got the same prune threshold. Now uses
                // futilityBase + PieceValueMG[victim] so small captures get
                // tighter gates. Gives qsearch the same gain-conditional
                // pruning SF18 uses. SHIPPED WITHOUT SPRT.
                PieceType victim = type_of(pos.piece_on(m.to_sq()));
                if (m.type_of() == MT_EN_PASSANT) victim = PAWN;
                // futilityBase: small slack above staticEval to absorb
                // post-capture quiet improvements. SF18 uses ~204; at 5x
                // scale that's ~1020. Pick the same magnitude as Hypersion's
                // QSEARCH futility slack uses elsewhere.
                Value futilityBase = staticEval + Value(150);
                Value gain         = Eval::PieceValueMG[victim];
                if (futilityBase + gain <= alpha) continue;
            }
        }

        StateInfo st;
        ss->currentMove = m;
        ss->movedPiece  = pos.piece_on(m.from_sq());
        pos.do_move(m, st);
        TT.prefetch(pos.key());
        pawnCorrHist.prefetch(pos.side_to_move(), pos.pawn_key());
        Value v = -qsearch(pos, ss + 1, -beta, -alpha, isPv);
        pos.undo_move(m);
        if (should_stop()) return VALUE_ZERO;

        if (v > bestValue) {
            bestValue = v;
            if (v > alpha) {
                bestMove = m;
                if (isPv && v < beta) alpha = v;
                else                  break;   // beta cutoff
            }
        }
    }

    if (inCheck && bestValue == -VALUE_INFINITE)
        return mated_in(ply);

    // 2026-05-17 qsearch finding #14/#29: SF18:1706-1707 averages a
    // non-decisive fail-high bestValue toward beta before TT-storing,
    // matching the same regress-to-beta smoothing the main search has.
    // Cuts down TT-write volatility from qsearch capture sequences.
    if (bestValue >= beta && std::abs(bestValue) < VALUE_TB_WIN_IN_MAX_PLY)
        bestValue = Value((bestValue + beta) / 2);

    // 2026-05-17 qsearch finding #31: SF18:1727 NEVER writes BOUND_EXACT
    // from qsearch — the search is incomplete (no quiets, no checks past
    // depth 0). An EXACT entry would let non-PV revisits TT-cut on a value
    // proven only inside the capture-only sub-tree. Use only LOWER/UPPER.
    //
    // 2026-05-17 qsearch finding #32: preserve sticky ttPv across qsearch
    // revisits. SF passes `pvHit = ttHit && ttData.is_pv` (already
    // computed in the local `ttPv`-like form earlier). Hypersion now
    // computes `qsearch_pvHit` from the same logic so the persistent
    // PV bit isn't overwritten by `isPv` on every qsearch save.
    Bound b = bestValue >= beta ? BOUND_LOWER : BOUND_UPPER;
    bool qsearch_pvHit = isPv || (ttHit && tte->is_pv());
    tte->save(pos.key(), TT.value_to_tt(bestValue, ply), qsearch_pvHit, b, 0,
              bestMove, staticEval, TT.generation());
    return bestValue;
}

// ---------------------------------------------------------------------------
// Main alpha-beta with PVS + TT + M4 pruning toolkit
// ---------------------------------------------------------------------------
Value Worker::search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth,
                     PVLine& pv, bool isPv, bool cutNode) {
    pv.length = 0;
    int ply = ss->ply;

    if (should_stop())   return VALUE_ZERO;
    if (depth <= 0)      return qsearch(pos, ss, alpha, beta, isPv);
    if (ply >= MAX_PLY)  return Eval::evaluate(pos);
    if (pos.is_draw(ply))
        return value_draw(nodes.load(std::memory_order_relaxed), limits.contempt);

    // 2026-05-17 audit #4: if a reversible-move repetition is reachable from
    // here AND we're losing (alpha < draw), bump alpha to VALUE_DRAW.
    // Catches "side to move can force perpetual" without actually searching
    // the repetition branch. SF18 search.cpp:630-635.
    if (ply > 0 && alpha < VALUE_DRAW && pos.upcoming_repetition(ply)) {
        alpha = value_draw(nodes.load(std::memory_order_relaxed), limits.contempt);
        if (alpha >= beta) return alpha;
    }

    // 2026-05-17 finding #8: SF18:670-671 updates `selDepth` per call so
    // the reported `info seldepth` reflects the true max ply explored
    // across all branches. Hypersion previously only updated selDepth at
    // qsearch entry — so non-PV branches that descend deep without entering
    // qsearch (e.g. via reductions getting clamped to 0) under-reported.
    if (ply + 1 > selDepth) selDepth = ply + 1;

    nodes.fetch_add(1, std::memory_order_relaxed);

    // Mate-distance pruning.
    alpha = std::max<int>(mated_in(ply), alpha);
    beta  = std::min<int>(mate_in(ply + 1),  beta);
    if (alpha >= beta) return alpha;

    bool inCheck   = pos.checkers();
    ss->inCheck    = inCheck;
    ss->moveCount  = 0;
    ss->statScore  = 0;       // SF18: cleared at entry, set per-move below.

    // 2026-05-18 Tier 2 (RubiChess threat-square HH): compute the most-
    // threatening enemy square once per node. Lookup at move-ordering
    // time blends with mainHist signal; update at cutoff writes back.
    // Definition: LSB of (pawn-attacks on our pieces excl. our pawns +
    // minor-attacks on our rooks/queens + rook-attacks on our queens).
    // Source: RubiChess board.cpp:346-378.
    {
        Color us = pos.side_to_move(), them = ~us;
        Bitboard occ = pos.pieces();
        Bitboard ourNonPawns = pos.pieces(us) & ~pos.pieces(us, PAWN);
        Bitboard pawnAtk = (them == WHITE)
            ? pawn_attacks_bb<WHITE>(pos.pieces(them, PAWN))
            : pawn_attacks_bb<BLACK>(pos.pieces(them, PAWN));
        Bitboard threats = pawnAtk & ourNonPawns;
        Bitboard ourMajors = pos.pieces(us, ROOK) | pos.pieces(us, QUEEN);
        Bitboard bb = pos.pieces(them, KNIGHT);
        while (bb) threats |= (PseudoAttacks[KNIGHT][pop_lsb(bb)] & ourMajors);
        bb = pos.pieces(them, BISHOP);
        while (bb) threats |= (attacks_bb<BISHOP>(pop_lsb(bb), occ) & ourMajors);
        bb = pos.pieces(them, ROOK);
        while (bb) threats |= (attacks_bb<ROOK>(pop_lsb(bb), occ) & pos.pieces(us, QUEEN));
        ss->threatSq = threats ? int(lsb(threats)) : 64;
    }
    // SF18: zero (ss+2)->cutoffCnt only — NOT ss->cutoffCnt itself. Our own
    // cutoffCnt accumulates across recursive calls from our parent's move
    // loop, which is precisely what makes the LMR heuristic work: parent
    // reads (parent_ss + 1)->cutoffCnt = OUR cutoffCnt to decide reductions
    // for its NEXT sibling move. (Our +2 frame is the slot a future
    // grandchild recursion will accumulate into; clearing it prepares that.)
    // Source: SF18 src/search.cpp:698-699.
    (ss + 2)->cutoffCnt = 0;

    // ---- TT probe ----
    bool ttHit = false;
    TTEntry* tte = TT.probe(pos.key(), ttHit);
    Value ttValue = ttHit ? TT.value_from_tt(tte->value(), ply, pos.rule50_count()) : VALUE_NONE;
    Move  ttMove  = ttHit ? tte->move() : Move::none();
    // NOTE: tested SF18 ttCapture flag for LMR — `if (ttCapture && m != ttMove)
    // ++r` (matching SF18 src/search.cpp:1204-1205 `r += 1119` ~= +1 ply).
    // Result: -17.4 +/- 36.1 ELO at 200g 5+0.05 (within noise band, but
    // negative point estimate). Hypothesis: over-reduction stacking with
    // the just-shipped cutoffCnt adjustment, which already adds +1..+2 plies
    // in cut-y subtrees. Adding another +1 in ttCapture cases compounds to
    // up to +3 ply extra reduction in worst case, which over-prunes the
    // tactical alternatives the heuristic tries to skip. SF's tuning has
    // both heuristics calibrated together; Hypersion's integer-ply rounding
    // amplifies each addition relative to SF's 1024ths-of-a-ply granularity.
    // Future contributor wanting to retry should:
    //   (a) gate ttCapture on `(ss+1)->cutoffCnt <= 2` to avoid stacking, or
    //   (b) make the cutoffCnt thresholds stricter (>3, >2 instead of >2, >1).
    // SF-style ttPv: position is "PV" if it's currently being searched as PV,
    // OR if the TT remembers it was once part of a PV. Sticky — encourages
    // less-aggressive pruning on positions that have ever been principal.
    //
    // 2026-05-17 finding #12: SF18:709 ALSO writes ttPv into the Stack so it
    // sticks across the recursive descent (`ss->ttPv = excludedMove ? ss->ttPv
    // : PvNode || (ttHit && ttData.is_pv)`). Without that write, children
    // can't inherit / parents can't bestow ttPv (used in finding #131
    // fail-low parent-bestow at SF18:1460-1461).
    bool  ttPv    = isPv || (ttHit && tte->is_pv());
    if (ss->excludedMove == Move::none())
        ss->ttPv = ttPv;

    // 2026-05-17 finding #13: SF18:710 computes `ttCapture` (true when the
    // TT move is a capture). It's referenced by several downstream SF
    // heuristics (LMR adjustment, RFP gate). Hypersion currently only uses
    // it in tombstone-cited LMR experiments; computing it costs ~1 ns/node
    // and keeps the inventory accurate so future ports don't have to
    // re-derive it.
    [[maybe_unused]] bool ttCapture = ttMove != Move::none() && pos.capture(ttMove);

    // 2026-05-17 CRITICAL FIX: TT cutoff must NOT fire when we're inside a
    // singular-extension recursion (ss->excludedMove != none). Otherwise the
    // singular search at search.cpp:2337 cuts immediately on the same TT
    // entry that triggered the SE check, making the singular test
    // tautologically pass and effectively disabling singular extension.
    // SF18 src/search.cpp:760 has this gate as `!excludedMove`.
    //
    // NOTE: SF18 also has an asymmetric depth comparison
    //   `ttData.depth > depth - (ttData.value <= beta)`
    // which is stricter than `>= depth` for fail-high cuts. Tried it and
    // bench jumped 1.3M -> 5.5M nodes (4x slowdown). Hypersion's other
    // tuning is calibrated around the symmetric `>= depth` gate, so the
    // SF-strict variant needs SPSA re-tuning to ship — reverted to `>=`.
    // 2026-05-17 audit #16: simple rule50 < 96 gate tested at 200g
    // bullet (5+0.05): -3.5 +/- 38.3 ELO (62W-64L-74D). The "don't cut
    // at high rule50" rule is theoretically correct but loses more
    // overall-search-speed than the rare-correctness-case it rescues.
    // REJECTED. SF's version pairs the gate with a depth-8 ttMove
    // verification probe — porting the full pair may behave better but
    // would need independent SPRT. Tombstoned for now.
    if (!isPv && ss->excludedMove == Move::none() && ttHit && tte->depth() >= depth) {
        if (tte->bound() == BOUND_EXACT
            || (tte->bound() == BOUND_LOWER && ttValue >= beta)
            || (tte->bound() == BOUND_UPPER && ttValue <= alpha)) {
            // 2026-05-17 audit #15: SF18 src/search.cpp:769-771 awards a
            // history bonus to the ttMove when a fail-high TT cutoff fires
            // with a quiet ttMove. Reinforces "this move historically cut
            // at this depth" for next-time move ordering.
            if (ttValue >= beta && !ttCapture
                && ttMove != Move::none() && ttMove != Move::null()) {
                Move  prevMove1Tc  = (ss - 1)->currentMove;
                Piece prevPiece1Tc = (ss - 1)->movedPiece;
                Move  prevMove2Tc  = (ss - 2)->currentMove;
                Piece prevPiece2Tc = (ss - 2)->movedPiece;
                update_quiet_history(pos, ttMove, history_bonus(depth),
                                     prevPiece1Tc, prevMove1Tc,
                                     prevPiece2Tc, prevMove2Tc);
            }
            return ttValue;
        }
    }

    // ---- Syzygy WDL probe ----
    // Cheap mid-search probe: when the position is small enough to be in TBs,
    // Fathom returns the truth and we can short-circuit.
    //
    // 2026-05-16 ply-adjusted TB score: previously probe_wdl returned a
    // constant Value(VALUE_TB_WIN - 100) regardless of search ply. Result:
    // search had no preference for short vs long wins, so once any winning
    // move was found the engine settled for it instead of pushing toward
    // mate.  SF (search.cpp:825) uses `VALUE_TB - ss->ply` — value decreases
    // with depth, so the search naturally prefers shorter wins.
    //
    // 2026-05-16 also widened the gate from `!isPv` to `ply > 0`: SF18
    // (search.cpp:803) probes TB at both PV and non-PV nodes (excluding
    // only the root). The previous `!isPv` skipped probes along the entire
    // PV chain — so the principal variation never got TB-win info, which
    // is exactly the chain that needs to report "mate-in-N" back to root.
    // Symptom: with Syzygy loaded, KBBK / KBNK mating took 43-49 moves
    // (vs 19/33 theoretical max) because the PV never saw the TB win.
    // 2026-05-17 finding #17: SF18:809 also requires `pos.rule50_count() == 0`
    // and `!pos.can_castle(ANY_CASTLING)` before probing — Fathom's WDL is
    // only valid when those gates pass. Without these gates Hypersion probed
    // castling/rule50-tainted positions and relied on Fathom returning FAIL.
    // 2026-05-17 audit #19/#130: TB-result PvNode maxValue / alpha clamp.
    // Previously when the TB lookup failed to cut off (PvNode wide window or
    // value strictly inside (alpha, beta)), the TB info was discarded and
    // search proceeded blind. SF18 (search.cpp:845-851) on PvNode keeps the
    // TB hint live: LOWER widens alpha (use as a floor), UPPER clamps
    // bestValue via maxValue at end-of-node. tbAlphaFloor/maxValue are
    // applied where bestValue exists (around the move loop / before TT write).
    Value maxValue      = VALUE_INFINITE;
    Value tbAlphaFloor  = -VALUE_INFINITE;   // PV-LOWER hint, applied to bestValue post-move-loop
    if (ply > 0 && depth >= Syzygy::probe_depth() && Syzygy::is_loaded()
        && pos.rule50_count() == 0
        && !pos.can_castle(WHITE_OO)  && !pos.can_castle(WHITE_OOO)
        && !pos.can_castle(BLACK_OO)  && !pos.can_castle(BLACK_OOO)) {
        Value tbVal = Syzygy::probe_wdl(pos);
        if (tbVal != VALUE_NONE) {
            // Replace flat TB-win/loss with ply-adjusted equivalents.
            // Range: just below mate scores, decreasing 1 cp per ply.
            if (tbVal >= Value(VALUE_TB_WIN - 200))
                tbVal = Value(VALUE_TB_WIN - ply);
            else if (tbVal <= Value(-VALUE_TB_WIN + 200))
                tbVal = Value(-VALUE_TB_WIN + ply);
            Bound b = tbVal >= VALUE_DRAW ? BOUND_LOWER : BOUND_UPPER;
            if ( b == BOUND_LOWER ? tbVal >= beta : tbVal <= alpha) {
                tte->save(pos.key(), TT.value_to_tt(tbVal, ply), false, b,
                          std::min<int>(depth + 6, MAX_PLY - 1),
                          Move::none(), VALUE_NONE, TT.generation());
                return tbVal;
            }
            // PvNode no-cutoff: keep the TB info live for end-of-node clamps.
            if (isPv) {
                if (b == BOUND_LOWER) {
                    tbAlphaFloor = tbVal;
                    if (tbVal > alpha) alpha = tbVal;
                } else {
                    maxValue = tbVal;
                }
            }
        }
    }

    Value rawEval, staticEval;
    if (inCheck) {
        // 2026-05-17 finding #21: SF18:716-717 propagates (ss-2)->staticEval
        // through in-check plies so `improving` and downstream margins still
        // have a meaningful eval after forcing-check sequences.
        Value carried = (ply >= 2 && (ss - 2)->staticEval != VALUE_NONE)
                          ? (ss - 2)->staticEval : VALUE_NONE;
        rawEval = staticEval = ss->staticEval = carried;
    } else if (ss->excludedMove != Move::none()) {
        // 2026-05-17 finding #22: SF18:718-719 reuses ss->staticEval inside
        // singular-extension recursion (excludedMove != none) — same position
        // key, same eval. Avoids re-running NNUE and keeps signal consistent
        // with the parent that performed the SE test.
        rawEval = staticEval = ss->staticEval;
    } else if (ttHit && tte->eval() != VALUE_NONE) {
        rawEval = tte->eval();
        // 2026-05-18 Tier 1 (Berserk-style corrhist blend): pawnCorrHist +
        // contCorrHist1 + contCorrHist2 additive blend. Weights are
        // Hypersion-internal (pawn=full, cont1=half, cont2=full) and bounded
        // by CORR_MAX*256 storage cap. SPRT will calibrate; defaults are
        // structurally similar to Berserk's 31/17/46/8192 ratio
        // (pawn ≈ cont2 > cont1).
        staticEval = pawnCorrHist.adjust(pos.side_to_move(), pos.pawn_key(), rawEval);
        if (ply >= 2 && (ss - 1)->currentMove != Move::none()
                     && (ss - 1)->currentMove != Move::null()
                     && (ss - 1)->movedPiece != NO_PIECE) {
            Piece innerPc = (ss - 1)->movedPiece;
            Square innerTo = (ss - 1)->currentMove.to_sq();
            int contAdjustCp = 0;
            if (ply >= 3 && (ss - 2)->currentMove != Move::none()
                         && (ss - 2)->currentMove != Move::null()
                         && (ss - 2)->movedPiece != NO_PIECE) {
                contAdjustCp += contCorrHist2.corr_cp((ss - 2)->movedPiece,
                                                     (ss - 2)->currentMove.to_sq(),
                                                     innerPc, innerTo);
            }
            if (ply >= 4 && (ss - 3)->currentMove != Move::none()
                         && (ss - 3)->currentMove != Move::null()
                         && (ss - 3)->movedPiece != NO_PIECE) {
                contAdjustCp += contCorrHist1.corr_cp((ss - 3)->movedPiece,
                                                     (ss - 3)->currentMove.to_sq(),
                                                     innerPc, innerTo) / 2;
            }
            staticEval = Value(int(staticEval) + contAdjustCp);
        }
        if (ttValue != VALUE_NONE
            && (((tte->bound() & BOUND_LOWER) && ttValue > staticEval)
                || ((tte->bound() & BOUND_UPPER) && ttValue < staticEval)))
            staticEval = ttValue;
        ss->staticEval = staticEval;
    } else {
        rawEval = Eval::evaluate(pos);
        staticEval = pawnCorrHist.adjust(pos.side_to_move(), pos.pawn_key(), rawEval);
        // 2026-05-18 Tier 1: same contCorrHist blend as above branch.
        if (ply >= 2 && (ss - 1)->currentMove != Move::none()
                     && (ss - 1)->currentMove != Move::null()
                     && (ss - 1)->movedPiece != NO_PIECE) {
            Piece innerPc = (ss - 1)->movedPiece;
            Square innerTo = (ss - 1)->currentMove.to_sq();
            int contAdjustCp = 0;
            if (ply >= 3 && (ss - 2)->currentMove != Move::none()
                         && (ss - 2)->currentMove != Move::null()
                         && (ss - 2)->movedPiece != NO_PIECE) {
                contAdjustCp += contCorrHist2.corr_cp((ss - 2)->movedPiece,
                                                     (ss - 2)->currentMove.to_sq(),
                                                     innerPc, innerTo);
            }
            if (ply >= 4 && (ss - 3)->currentMove != Move::none()
                         && (ss - 3)->currentMove != Move::null()
                         && (ss - 3)->movedPiece != NO_PIECE) {
                contAdjustCp += contCorrHist1.corr_cp((ss - 3)->movedPiece,
                                                     (ss - 3)->currentMove.to_sq(),
                                                     innerPc, innerTo) / 2;
            }
            staticEval = Value(int(staticEval) + contAdjustCp);
        }
        ss->staticEval = staticEval;
        // 2026-05-17 finding #24: SF18:736-741 writes the first-visit eval to
        // TT with BOUND_NONE so subsequent re-visits skip the NNUE forward
        // pass. Cheap caching of the most expensive eval-side work. We skip
        // when ttHit (the TT entry exists and we already used tte->eval())
        // or when in SE recursion (don't overwrite the parent's entry).
        if (!ttHit && ss->excludedMove == Move::none())
            tte->save(pos.key(), VALUE_NONE, ttPv, BOUND_NONE,
                      0, Move::none(), rawEval, TT.generation());
    }

    // NOTE: tested SF18 priorReduction hindsight depth bump: parent records
    // its LMR amount at (ss-1)->reduction; child reads at entry and bumps
    // own depth by 1 if reduction was >= 3 plies (under-search compensation,
    // matching SF18 src/search.cpp:753-754).
    // Result: -120.4 +/- 93.7 ELO at 30g 5+0.05 (clear regression).
    //
    // Diagnosis: directly anti-synergistic with the just-shipped cutoffCnt
    // LMR adjustment (commit 820c06f). cutoffCnt INCREASES reduction in
    // cut-y subtrees (where many siblings already failed high). priorReduction
    // then BUMPS DEPTH BACK UP at children that were heavily reduced — so
    // the cutoffCnt savings are immediately spent re-searching the very
    // children it tried to skip. Net: same node count as without cutoffCnt
    // but with worse priors (we're now committing extra work to leaves the
    // sibling-cutoff signal told us were unlikely to be principal).
    //
    // To retry: would need to pair priorReduction with weakened cutoffCnt
    // thresholds (e.g. only the > 2 case, drop the allNode path) so the two
    // heuristics don't cancel. Or skip priorReduction entirely — SF gates it
    // with !opponentWorsening which we don't have, and the SF tuning may
    // depend on that gate to keep the bump-up rate low enough to win.
    // Stack::reduction field kept (cheap) so a future contributor can wire
    // it back in without re-laying the plumbing.
    // NOTE: round-6 added a parallel materialCorrHist with averaged
    // contributions. Result: -26 ELO at 200 games. Two issues: the
    // 14-bit material key has heavy bucket collisions (many dissimilar
    // positions share each bucket, so the signal is noisy), and the
    // averaging logic halved the existing pawn-correction strength.
    // Kept the materialCorrHist field so a future contributor can try
    // a better integration (additive with weight, or a separate small
    // bonus) without re-laying the plumbing.

    // "Improving": did our static eval go up since 2 plies ago? Used to soften pruning
    // when our position is getting better (we have time to wait for a real refutation).
    bool improving = false;
    if (!inCheck) {
        if (ply >= 2 && (ss - 2)->staticEval != VALUE_NONE)
            improving = staticEval > (ss - 2)->staticEval;
        else if (ply >= 4 && (ss - 4)->staticEval != VALUE_NONE)
            improving = staticEval > (ss - 4)->staticEval;
        else
            // 2026-05-17 SF18 src/search.cpp:716,750: when in-check propagates
            // the chain, the broken-chain fallback is `improving = false`.
            // Hypersion previously defaulted to `true` (gentler pruning) when
            // both lookbacks were unavailable, causing under-pruning in long
            // forcing-check sequences. Match SF's conservative default.
            improving = false;
    }

    // SF18 opponentWorsening flag (src/search.cpp:751). Computed but not yet
    // wired into any pruning margin in Hypersion — kept as scaffolding for a
    // future port. `[[maybe_unused]]` silences -Wunused-but-set-variable.
    // When we wire this in, gate via SPSA: any aggressive use of the flag
    // touches LMR/RFP margins that are jointly tuned (see history.h tombstones).
    [[maybe_unused]] bool opponentWorsening = false;
    if (!inCheck && ply >= 1 && (ss - 1)->staticEval != VALUE_NONE)
        opponentWorsening = staticEval > -(ss - 1)->staticEval;

    // NOTE: 2026-05-12 tested SF18:859-867 eval-diff history bonus port
    // (update opponent's mainHistory at node entry based on -(prev_eval +
    // cur_eval) clamped + scaled). Hypersion 5x-scale port with bonus
    // multiplier 9/5: -88.7 ± 165 ELO @ 12g. Direct port regresses for
    // same local-optimum reason as opponentWorsening RFP. Reverted.

    // NOTE: rounds 7/8/10/12 added an `opponentWorsening` flag and used it
    // in RFP, futility, ProbCut, and razoring margins. Each individual A/B
    // showed strong positive results (+33, +117, +35, +102), but a final
    // direct measurement of the cumulative state vs the round-2 binary
    // gave -161.9 ELO at 200 games — the chain inference had been
    // poisoned by random-opening selection variance across cutechess
    // matches. With proper opening control (a fixed PGN/EPD subset), the
    // signal might still be there, but the current codebase is reverted
    // back to the pre-opponentWorsening state. See round-7..-12 entries
    // in testing/IMPROVEMENTS_LOG.md.

    // ---- Reverse Futility Pruning (Static Null-Move) ----
    // NOTE: 2026-05-12 opponentWorsening RFP margin reduction tested TWICE:
    //   825 cp: -114 ± 118 ELO @ 22g (way too aggressive)
    //    40 cp: -44 ± 94 ELO @ 40g (still negative, SF-calibrated mag)
    // Even SF's mathematical-equivalent magnitude fails in Hypersion. The
    // local-optimum hypothesis is confirmed: SF's RFP works with its full
    // pruning stack (5 corrhists, ttHit-adjusted futilityMult, etc); the
    // opponentWorsening sub-term can't be ported in isolation. SF docs
    // confirm individual heuristics are tested at fishtest with tens of
    // thousands of games against the full engine — single-feature ports
    // to differently-tuned engines often fail.
    // 2026-05-16 threshold widened to TB-decisive: SF18 src/search.cpp:887
    // uses `!is_loss(beta) && !is_win(eval)`. The is_loss/is_win pair
    // covers TB-magnitude scores, not just true mates. Symmetric-conservative
    // form below matches: skip RFP whenever beta or eval is TB-decisive.
    if (!isPv && !inCheck && depth <= 7
        && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
        && staticEval < VALUE_TB_WIN_IN_MAX_PLY
        && staticEval - RFP_MARGIN_PER_DEPTH * (depth - improving) >= beta)
        // 2026-05-17 finding #30: SF18:888 returns `(2*beta + eval) / 3`
        // (weighted moderation toward beta), not raw `staticEval`. Raw
        // staticEval over-claimed the cutoff score, propagating inflated
        // values into TT on the RFP path.
        return Value((2 * beta + staticEval) / 3);

    // ---- Razoring ----
    // NOTE: tried bumping depth ceiling 4 -> 5; -12.2 +/- 36.6 ELO
    // at 200g 5+0.05.  Within noise but mildly negative; razoring at
    // depth 5 mis-cuts more often than the savings justify here.
    if (!isPv && !inCheck && depth <= 4
        && staticEval + RAZOR_MARGIN_BASE + RAZOR_MARGIN_PER_DEPTH * depth <= alpha) {
        // 2026-05-17 finding #35: SF18:874 razor probe uses the FULL search
        // window (alpha, beta). Hypersion's null-window `(alpha, alpha+1)`
        // can return a tighter bound than warranted, biasing the razor
        // verify-step. Full window matches SF.
        Value v = qsearch(pos, ss, alpha, beta, /*isPv=*/false);
        if (v <= alpha) return v;
    }

    // ---- Null Move Pruning ----
    // Zugzwang protection: at high depths require strictly more non-pawn
    // material than a single minor piece, where zugzwang false-positives
    // are most damaging (loss is amplified by the depth saved). Below the
    // depth threshold a single minor still permits NMP — the cost of a
    // wrong cut is cheap. At low depths even K+P NMP is fine because the
    // verification cost of a re-search is small. Endgame analysis of the
    // 50g vs SF match showed 70 % of blunders were in endgame; this cuts
    // the worst tree-pruning bug among them.
    int npMat = int(pos.non_pawn_material(pos.side_to_move()));
    bool nmpMaterialOk = (depth < 12) ? (npMat > 0)
                                       : (npMat > 781);   // > knight value
    // NOTE: round 11 tried `staticEval >= beta - 100 * opponentWorsening`
    // to loosen NMP entry on positive trends. Result: -12.2 ELO at 200
    // games. Likely cause: NMP already fires very often, the borderline
    // entries added by the 100 cp slack mostly fail-low and waste work.
    // NOTE: tried adding SF18 NMP verification search at depth >= 16 with
    // nmpMinPly tracking (matching SF master src/search.cpp:909). Result:
    // -20.9 +/- 38.3 ELO at 200g 5+0.05. The verification adds search
    // work at high-depth NMP cutoffs without measurable accuracy gain in
    // Hypersion — its existing material+depth guards already catch the
    // dangerous false-cutoff cases. Reverted.
    // UCI_AnalyseMode skips NMP: forcing-line analysis can miss zugzwang
    // mate-threats when NMP cuts the entire ZW branch. At analysis time
    // the cost of being thorough is acceptable.
    // NOTE: 2026-05-12 tested SF18-style NMP gate `cutNode` + base R=7
    // (drop eval-beta term) + capHist 1.13x update scaling + dynamic
    // ProbCut depth + multi-corrhist expansion as a bundle. LTC 20g @
    // TC 30+0.3 vs prior shipped: -34.9 +/- 111.3 ELO (4W-6L-10D).
    // CI crosses 0 but trajectory dropped from +50 ELO at game 13 to
    // -34.9 final — late games lost. Conversion test was flat at 4/6.
    // REVERTED back to single-port stack.
    // 2026-05-16 threshold widened to TB-decisive: SF18 src/search.cpp:894
    // uses `!is_loss(beta)`. Skip NMP when beta is TB-magnitude in either
    // direction (the symmetric form is slightly more conservative than SF's
    // directional but equivalently safe — TB-magnitude beta makes NMP
    // semantics degenerate).
    if (!isPv && !inCheck && depth >= 3
        && !limits.analyseMode
        && (ss - 1)->currentMove != Move::null()
        && ss->excludedMove == Move::none()
        && staticEval >= beta
        && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
        && nmpMaterialOk) {

        int R = 4 + depth / 3 + std::min(3, int(staticEval - beta) / NMP_EVAL_BETA_DIV);
        StateInfo st;
        ss->currentMove = Move::null();
        ss->movedPiece  = NO_PIECE;
        pos.do_null_move(st);
        TT.prefetch(pos.key());
        pawnCorrHist.prefetch(pos.side_to_move(), pos.pawn_key());
        PVLine dummyPv;
        Value nullValue = -search(pos, ss + 1, -beta, -beta + 1, depth - R, dummyPv,
                                  /*isPv=*/false, !cutNode);
        pos.undo_null_move();
        if (should_stop()) return VALUE_ZERO;
        if (nullValue >= beta) {
            // Don't return unproven decisive scores — fall back to plain beta.
            // 2026-05-16 EXTENDED: previously only rejected true-mate values.
            // TB-win values (>= VALUE_TB_WIN_IN_MAX_PLY) also need to be
            // rejected because NMP didn't actually verify the TB win — it
            // just got a high score from a reduced-depth search that may
            // have touched TBs in a subtree. Returning such a score
            // propagates an unverified TB-win up the tree (and into TT).
            // Source: SF18 src/search.cpp:907 (`!is_win(nullValue)`).
            return nullValue >= VALUE_TB_WIN_IN_MAX_PLY ? beta : nullValue;
        }
    }

    // ---- ProbCut ----
    // At a non-PV node with depth >= 5, scan captures whose SEE clears the bar
    // toward beta + margin. If a capture reaches that bar via a real reduced
    // search, prune the whole subtree.
    // NOTE: tested lowering threshold to depth >= 4 (matching SF18 closer at
    // depth >= 3). Trajectory:
    //   30g:  +34.9 +/- 96.0 ELO  (noise band, mildly positive)
    //   200g: +6.9  +/- 36.4 ELO  (between reject +5 and ship +10)
    //   300g: -1.2  +/- 30.6 ELO  (REJECT, converged to ~0)
    // Same fakeout pattern as SE depth++. Lowering threshold adds ~20% more
    // ProbCut attempts at depth 4 but the cost (re-search on probcut hit)
    // doesn't pay back at this calibration.
    // 2026-05-16 threshold widened to TB-decisive: SF18 src/search.cpp:940
    // uses `!is_decisive(beta)`. ProbCut compares against beta+margin, so
    // beta in TB range would push probCutBeta into invalid territory.
    if (!isPv && !inCheck && depth >= 5
        && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
        && ss->excludedMove == Move::none()) {
        // NOTE: SF18-style dynamic probCutDepth (depth - 5 - (eval-beta)/315)
        // tested 2026-05-12 bundled with NMP changes; LTC cumulative -34.9
        // ELO. Reverted to fixed depth - 4.
        Value probCutBeta = std::min<int>(beta + PROBCUT_MARGIN, VALUE_INFINITE - 1);
        MovePicker pcMp(pos, ttMove, &mainHist, &captureHist, /*qDepth=*/0);
        Move m;
        while ((m = pcMp.next_move()) != Move::none()) {
            if (m == ss->excludedMove) continue;
            if (!pos.capture(m)) continue;             // ProbCut explores noisy moves
            if (!pos.legal(m)) continue;
            if (!pos.see_ge(m, Value(probCutBeta - staticEval))) continue;

            StateInfo st;
            ss->currentMove = m;
            ss->movedPiece  = pos.piece_on(m.from_sq());
            pos.do_move(m, st);
            TT.prefetch(pos.key());
            pawnCorrHist.prefetch(pos.side_to_move(), pos.pawn_key());
            // Verify with a quick null-window qsearch first.
            Value v = -qsearch(pos, ss + 1, -probCutBeta, -probCutBeta + 1, /*isPv=*/false);
            // If the quick check says the cut might hold, confirm with a real reduced search.
            if (v >= probCutBeta) {
                PVLine dummyPv;
                v = -search(pos, ss + 1, -probCutBeta, -probCutBeta + 1, depth - 4, dummyPv,
                            /*isPv=*/false, !cutNode);
            }
            pos.undo_move(m);
            if (should_stop()) return VALUE_ZERO;
            if (v >= probCutBeta) {
                // SF18 src/search.cpp:977-978: ProbCut confirmed `v >=
                // probCutBeta` (= beta + margin), but the reduced search only
                // proved "score is at least probCutBeta". Returning v as-is
                // claims the full margin's worth of score even when v was
                // only barely above the threshold. SF returns `v - (probCutBeta
                // - beta)` which subtracts the margin back so the returned
                // score doesn't overclaim. Pre-2026-05-12 Hypersion returned
                // v directly — too optimistic, especially when TT stores it.
                // 2026-05-16 threshold widened: SF18 src/search.cpp:977
                // uses `!is_decisive(value)` so TB-win values stay AS-IS
                // (don't get their margin subtracted, which would underflow
                // the TB-win range and silently corrupt the score before
                // TT-write). Critical: without this, TB-win results from
                // ProbCut feed back into TT as below-TB-win values.
                // 2026-05-17 finding #49: SF18:973-975 writes the ProbCut
                // result to TT before returning so revisits hit the cache
                // and skip the inner search entirely. Bound = LOWER (we
                // proved v >= probCutBeta), depth = `depth - 3` (one less
                // than the reduced search to stay conservative).
                Value retVal = (std::abs(v) < VALUE_TB_WIN_IN_MAX_PLY)
                                 ? Value(int(v) - int(probCutBeta - beta))
                                 : v;
                if (ss->excludedMove == Move::none())
                    tte->save(pos.key(), TT.value_to_tt(retVal, ply), ttPv,
                              BOUND_LOWER, std::max(0, depth - 3), m,
                              rawEval, TT.generation());
                return retVal;
            }
        }
    }

    // ---- Small ProbCut (SF18 src/search.cpp:985-989) ----
    // TT-based fail-high shortcut: when the TT entry is a LOWER bound at
    // sufficient depth AND its stored value is well above beta (by 418 cp
    // in SF's scale = ~2000 in Hypersion's 5x scale), we can return early
    // without searching. Cheap check, no recursive search needed. Codex
    // flagged this as untested in Hypersion's prior audit.
    //
    // 2026-05-12: ported with classical-eval-aware port (NNUE-off testing
    // mode); will A/B vs prior shipped at fast TC + endgame conversion.
    {
        Value smallProbCutBeta = std::min<int>(beta + 2090, VALUE_INFINITE - 1);
        if (!isPv && !inCheck
            // 2026-05-17 finding #51: SF18:988 uses `(ttData.bound & BOUND_LOWER)`
            // which matches BOTH BOUND_LOWER and BOUND_EXACT (the latter has the
            // LOWER bit set). Hypersion's `==` missed BOUND_EXACT entries,
            // narrowing the small-ProbCut cutoff opportunity.
            && ttHit && (tte->bound() & BOUND_LOWER)
            && tte->depth() >= depth - 4
            && ttValue != VALUE_NONE && ttValue >= smallProbCutBeta
            // 2026-05-16 thresholds widened to TB-decisive: SF18
            // src/search.cpp:988 uses `!is_decisive(beta) && !is_decisive(ttData.value)`
            && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
            && std::abs(ttValue) < VALUE_TB_WIN_IN_MAX_PLY)
        {
            return smallProbCutBeta;
        }
    }

    // ---- Internal Iterative Reductions ----
    // PV/cut nodes without a TT move are likely to need the extra work — but
    // searching at the requested depth is wasteful. Reduce by 1.
    // NOTE: tested raising IIR threshold from depth>=4 to depth>=6 (matching
    // SF18 src/search.cpp:932) post-cutoffCnt-ship.  Result: +0.0 +/- 92.8
    // ELO at 30g 5+0.05 — pure noise. Threshold change doesn't matter at
    // current calibration. Kept at 4. Future contributor wanting to
    // re-tune should also try the SF gate `priorReduction <= 3` paired
    // with priorReduction's plumbing being live.
    if ((isPv || cutNode) && depth >= 4 && ttMove == Move::none())
        depth -= 1;

    // SF18 src/search.cpp:927 — upgrade `improving` to also be true when
    // the current static eval is at or above beta, not just when it grew
    // from 2/4 plies ago. The original `improving` only flips true if the
    // STM's eval trajectory is rising; this catches the case where eval is
    // already strong (>= beta) but happens to be flat or even slightly
    // worsening over recent plies. Used downstream by LMP (lmp_threshold
    // takes `improving`) and per-move pruning margins. Gentler pruning in
    // good positions = fewer false cutoffs at depth.
    improving |= staticEval >= beta;

    // Counter-move and previous-move bookkeeping for continuation history lookups.
    Move  prevMove1  = (ss - 1)->currentMove;
    Piece prevPiece1 = (ss - 1)->movedPiece;
    Move  prevMove2  = (ss - 2)->currentMove;
    Piece prevPiece2 = (ss - 2)->movedPiece;
    Move  counter    = (prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none())
                       ? counterMoves.get(prevPiece1, prevMove1.to_sq())
                       : Move::none();

    PVLine childPv;
    MovePicker mp(pos, ttMove, &mainHist, &captureHist, killers.killers[ply], depth,
                  contHist[0].get(), prevMove1, prevPiece1,
                  contHist[1].get(), prevMove2, prevPiece2,
                  &threatHist, ss->threatSq);

    Value bestValue = -VALUE_INFINITE;
    Move  bestMove  = Move::none();
    int   moveCount = 0;
    // 2026-05-17 audit #19/#130: apply TB LOWER-bound hint as initial bestValue
    // floor so weak quiet moves can't unseat the proven TB-win-value baseline.
    if (tbAlphaFloor > -VALUE_INFINITE)
        bestValue = tbAlphaFloor;
    // 2026-05-17 finding #57: bumped from 64 / 32 to 128 / 64 so pathological
    // positions (many promotion-captures + many quiets) don't silently lose
    // history malus updates past index 63 / 31. SF uses ~SEARCHEDLIST_CAPACITY
    // (similar magnitude); we err on the safe side since these arrays are
    // stack-allocated and cheap at MAX_MOVES range.
    Move  quietsTried[128];
    int   quietCount = 0;
    Move  capturesTried[64];
    int   captureCount = 0;
    bool  skipQuiets = false;

    Move m;
    while ((m = mp.next_move(skipQuiets)) != Move::none()) {
        if (m == ss->excludedMove) continue;
        if (!pos.legal(m)) continue;
        ++moveCount;
        ss->moveCount = moveCount;

        bool isCapture = pos.capture(m);
        Piece moving   = pos.piece_on(m.from_sq());
        bool givesCheck = pos.gives_check(m);

        // ---- Move-count pruning at low depth (LMP) ----
        // Skip LMP entirely in analyse mode — late quiet moves can still
        // be the mate-leading move in deep tactical lines.
        // 2026-05-16 thresholds widened to TB-decisive (SF18:1051 `!is_loss`).
        if (!isPv && !inCheck && bestValue > VALUE_TB_LOSS_IN_MAX_PLY && depth <= 8
            && !limits.analyseMode
            && moveCount > lmp_threshold(depth, improving)) {
            skipQuiets = true;
        }

        // ---- Pruning at low depth ----
        // Skip all shallow-depth pruning in analyse mode for thoroughness.
        if (!isPv && !inCheck && bestValue > VALUE_TB_LOSS_IN_MAX_PLY
            && !limits.analyseMode) {
            if (!isCapture && !givesCheck) {
                // Futility pruning for quiets.
                // 2026-05-17 finding #66: SF18:1103 `continue`s the current
                // move only; Hypersion's `skipQuiets = true` cancelled ALL
                // subsequent quiets including ones with better future-side
                // futility margins. Per-move continue restores correctness.
                //
                // 2026-05-17 7th-pass audit: tested adding SF18's protective
                // bonuses (`+ 161*!bestMove + 85*(staticEval>alpha)`,
                // Hypersion 5x scale = +805/+425) to make pruning less
                // aggressive in the protected cases. 30g triage: -34.9 +/-
                // 107.6 ELO (9W-12L-9D). Tombstoned: Hypersion's current
                // futility tuning is already at a local optimum; the SF
                // bonuses reduce prune rate without recouping accuracy.
                if (depth <= 6 && staticEval + FUTIL_MARGIN_PER_DEPTH * depth + FUTIL_MARGIN_BASE <= alpha)
                    continue;
                // SEE pruning of bad quiets.
                // NOTE: round 13 tried `(depth - opponentWorsening)` here.
                // Result: -47 ELO at 200 games. Reinforces the round-9
                // lesson: opponentWorsening helps subtree elision but
                // hurts per-move within-subtree decisions.
                if (depth <= 8 && !pos.see_ge(m, Value(SEE_QUIET_MARGIN * depth)))
                    continue;
                // NOTE: SF18-style continuation-history pruning was
                // attempted with threshold -4097*depth but regressed
                // -207 ELO at 86 games.  Hypersion's history-value
                // scale is different — needs a per-codebase threshold
                // search before this can be re-tried.  See round-4
                // notes in testing/IMPROVEMENTS_LOG.md.
            } else {
                // SEE pruning of bad captures.
                if (depth <= 6 && !pos.see_ge(m, Value(SEE_CAPT_MARGIN * depth)))
                    continue;
            }
        }

        // ---- Singular extension ----
        // If the TT move looks uniquely good (a reduced search excluding it
        // can't reach a lower beta), extend by one ply to verify.
        // Stockfish 18 uses depth >= 5; was 6 in earlier Hypersion. Lowering
        // adds ~5 % more SE attempts at near-leaf nodes which is where the
        // depth amplification matters most for forcing-line discovery.
        Depth extension = 0;
        // 2026-05-16 threshold widened to TB-decisive: SF18:1130 uses
        // `!is_decisive(ttData.value)`. SE shouldn't fire when the TT value
        // is TB-magnitude — singularBeta = ttValue - small offset would
        // still be in TB range, defeating the singular test.
        if (depth >= 5
            && m == ttMove
            && ss->excludedMove == Move::none()
            && std::abs(ttValue) < VALUE_TB_WIN_IN_MAX_PLY
            && (tte->bound() & BOUND_LOWER)
            && tte->depth() >= depth - 3
            && ply > 0
            && !is_shuffling(m, ss, pos)) {  // post-SF18: skip SE on shuffle
            // singularBeta = ttValue - depth * 2.  Tested depth*3:
            //   -6.9 +/- 38 ELO at 200g 5+0.05 (within noise, mildly
            // negative).  Kept at 2.
            Value singularBeta = Value(ttValue - depth * 2);
            Depth singularDepth = (depth - 1) / 2;
            ss->excludedMove = m;
            PVLine dummyPv;
            Value v = search(pos, ss, singularBeta - 1, singularBeta, singularDepth,
                             dummyPv, /*isPv=*/false, cutNode);
            ss->excludedMove = Move::none();
            if (should_stop()) return VALUE_ZERO;
            if (v < singularBeta) {
                extension = 1;          // singular — extend
                // NOTE: tested SF18 src/search.cpp:1151 SE depth++ — when
                // singularity is detected, also bump the current depth by 1
                // for the whole subtree.  Result trajectory:
                //   30g triage: +70.4 +/- 93.3 ELO  (looked PROMISING)
                //   200g confirm: +10.4 +/- 37.7 ELO  (at ship bar)
                //   300g extended: -4.6 +/- 30.3 ELO  (REJECT, tombstone)
                // Classic PROTOCOL.md fakeout pattern (+70 -> +10 -> -5).
                // The depth++ adds significant work on ~1% of nodes (SE
                // firing rate), and the extra subtree depth doesn't pay
                // back in this codebase's calibration.  SF gates this with
                // their full doubleMargin/tripleMargin family which we
                // don't have; trying just depth++ in isolation under-
                // performs because we lack the compensating rate-control.
                // Future contributor wanting to retry should pair with
                // SF's doubleMargin/tripleMargin tunables (require
                // ttCapture flag + ttPv tracking + correctionValue
                // computation, all SF18 src/search.cpp:1142-1149).
            } else if (singularBeta >= beta) {
                // Multi-cut: another move already meets beta in the reduced search,
                // so the position is at least beta — return early.
                // NOTE: tried broadening to SF18's `v >= beta` condition (which is
                // more inclusive), result -22.6 +/- 36 ELO at 200g 5+0.05.
                // Hypersion's stricter `singularBeta >= beta` heuristic is
                // calibrated for its specific parameter set; the broader
                // SF version triggers spurious multi-cuts here.
                return singularBeta;
            }
        }
        // In-check move that's not yet at a mate-distance edge — small extension.
        // NOTE: tested lowering depth threshold from >= 8 to >= 6.
        //   30g:  -46.6 +/- 110.7  (noise)
        //   100g:   0.0 +/- 54.0   (recovered to neutral)
        //   200g: -15.6 +/- 38.2   (REJECT, just outside noise band)
        // Extra extensions at depth 6-7 cost more search work than they
        // recover in tactical accuracy at this calibration. Kept at >= 8.
        else if (givesCheck && depth >= 8 && !inCheck) {
            extension = 1;
        }
        // Recapture extension: if the previous move ended on `to` and we're
        // recapturing, the capture sequence is forced — go a ply deeper.
        else if (isCapture && depth >= 6 && (ss - 1)->currentMove != Move::none()
                 && (ss - 1)->currentMove != Move::null()
                 && (ss - 1)->currentMove.to_sq() == m.to_sq()) {
            extension = 1;
        }
        // Passed-pawn-to-7th extension: pawn moves that reach the rank just
        // before promotion are usually critical and worth searching deeper.
        else if (type_of(moving) == PAWN
                 && rank_of(m.to_sq()) == (pos.side_to_move() == WHITE ? RANK_7 : RANK_2)
                 && depth >= 5) {
            extension = 1;
        }

        StateInfo st;
        ss->currentMove = m;
        ss->movedPiece  = moving;
        pos.do_move(m, st, givesCheck);
        TT.prefetch(pos.key());
        pawnCorrHist.prefetch(pos.side_to_move(), pos.pawn_key());

        // ---- Late Move Reductions ----
        Depth newDepth = depth - 1 + extension;
        // 2026-05-17 SF18 src/search.cpp:1283-1287: when following the
        // ttMove on a PV node and the TT entry is either decisive or
        // deep enough, floor newDepth at 1. Prevents dropping straight to
        // qsearch on a ttMove that has a known mate-line continuation,
        // which caused mate-finding losses near horizon.
        if (isPv && m == ttMove
            && ((ttValue != VALUE_NONE && std::abs(ttValue) >= VALUE_TB_WIN_IN_MAX_PLY
                 && tte->depth() > 0)
                || tte->depth() > 1))
            newDepth = std::max<int>(newDepth, 1);
        Depth r = 0;
        if (depth >= 3 && moveCount > 1 + (isPv ? 1 : 0) && (!isCapture || cutNode)) {
            r = lmr_base(depth, moveCount);
            // NOTE: 2026-05-16 v32 tried `if (depth >= 18 && r > 0) --r;`
            // to fix the v18-LMR-1.87 LTC asymmetry. Result was the
            // OPPOSITE of the hypothesis:
            //   bullet TC 5+0.05, 200g: +41.9 +/- 39.7 ELO  (LOS 98.1%)
            //   LTC    TC 20+0.2, 100g: -49.0 +/- 54.2 ELO  (LOS 3.8%)
            // Softening LMR at deep depths *helps* bullet (which reaches
            // depth >= 18 in late-midgame quiet positions and benefits
            // from wider search there) but *hurts* LTC (which has more
            // genuinely-deep nodes that should be pruned harder for time
            // efficiency). Net: same TC asymmetry, opposite sign. The
            // LTC negative is too large to combine with bullet positive,
            // so REJECT. Future contributor investigating LTC asymmetry
            // should try the OPPOSITE intervention: tighten LMR at depth
            // >= 18 (i.e. `if (depth >= 18) ++r;`). The data says LTC
            // currently under-prunes at deep nodes.
            if (!improving)        ++r;
            // NOTE: round 9 added `if (opponentWorsening) ++r;` here.
            // Result: -33 ELO at 200 games. RFP and futility both work
            // well with opponentWorsening because they affect tree
            // ENTRY/EXIT (full subtree elision); LMR works on depth WITHIN
            // a subtree where over-reducing critical late moves costs
            // more than the saved nodes are worth.
            if (cutNode)           r += 2;
            if (isPv)              --r;
            if (ttPv)              --r;   // SF-style: TT-remembered PV positions reduced less
            if (givesCheck)        --r;
            if (ttMove == m)       --r;
            if (m == counter)      --r;
            if (isCapture)         --r;   // captures shouldn't be reduced as much

            // Endgame LMR mitigation: when the position has very few pieces,
            // reduce by one less. Endgames need accurate calculation of long
            // forcing lines (passed-pawn races, K+P chasing, opposition) and
            // LMR is much more likely to truncate critical lines when a
            // single tempo flips the result. The 50g vs SF match analysis
            // showed 70 % of Hypersion blunders happened in endgame — many
            // attributable to over-reduced lines that hid the refutation.
            if (popcount(pos.pieces()) <= 8) --r;

            // SF18 cutoffCnt LMR adjustment. When the previously-searched
            // sibling moves' subtrees produced many fail-highs in our child
            // ply, the position is cut-off-heavy — reduce more aggressively
            // for the current move (we're likely to cut off too, so verify
            // with less depth). Conservative integer-ply port of SF's
            // fixed-point version (which uses +256 / +1024 ply-1024ths):
            //   SF: r += 256 + 1024*(cnt>2) + 1024*allNode  if cnt > 1
            // Hypersion approximation:
            //   if cnt > 2:           +1 ply
            //   if allNode && cnt>1:  +1 ply
            // Source: SF18 src/search.cpp:1208-1209.
            if ((ss + 1)->cutoffCnt > 2) ++r;
            if ((ss + 1)->cutoffCnt > 1 && !isPv && !cutNode) ++r;
            // (See ttCapture tombstone above TT probe.)

            // Stockfish-18 LMR history correction. High-history quiet moves get
            // reduced less; low-history get reduced more. Sums the same signals
            // MovePicker uses for quiet ordering: butterfly (×2) + 1-ply contHist
            // + 2-ply contHist. SF tunes with /11248 against a 4-ply contHist
            // sum; with only 2-ply we use /8192 to keep the effective reduction
            // range similar (~ ±2 plies at the extremes for ±16k statScore).
            if (!isCapture) {
                int statScore = 2 * mainHist.get(color_of(moving), m);
                if (prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none())
                    statScore += contHist[0]->get(prevPiece1, prevMove1.to_sq(), moving, m.to_sq());
                if (prevPiece2 != NO_PIECE && prevMove2 != Move::null() && prevMove2 != Move::none())
                    statScore += contHist[1]->get(prevPiece2, prevMove2.to_sq(), moving, m.to_sq());
                r -= statScore / LMR_STATSCORE_DIV;   // A5: pre-tunable was hardcoded 8192
            } else {
                // 2026-05-17 finding #96: SF18:1216-1217 gives captures a
                // statScore too, using captureHistory + a piece-value term
                // — captures with strong captHist get reduced less. Without
                // this, all captures get the same r at the same depth,
                // losing the historical-quality discrimination.
                PieceType victim = type_of(pos.piece_on(m.to_sq()));
                if (m.type_of() == MT_EN_PASSANT) victim = PAWN;
                int statScore = captureHist.get(moving, m.to_sq(), victim)
                              + int(Eval::PieceValueMG[victim]) * 4;
                r -= statScore / LMR_STATSCORE_DIV;
            }
            r = std::clamp(r, 0, newDepth - 1);
            // Analyse mode: halve the LMR aggression to find forcing lines
            // that LMR would otherwise hide. Captures, checks, and TT moves
            // already get reduced bonuses above; this halves the residual.
            if (limits.analyseMode && r > 0) r /= 2;
        }
        // Record how much we reduced for SF18-style hindsight depth adjustment
        // at the child's entry. r=0 for first move (no LMR) and full-window
        // re-searches; otherwise r > 0 means we under-searched the child.
        // Source: SF18 src/search.cpp:1240-1242.
        ss->reduction = r;

        Value v;
        if (moveCount == 1) {
            v = -search(pos, ss + 1, -beta, -alpha, newDepth, childPv, isPv, false);
        } else {
            // Reduced null-window search.
            int reducedDepth = newDepth - r;
            v = -search(pos, ss + 1, -alpha - 1, -alpha, reducedDepth, childPv,
                        /*isPv=*/false, true);
            // SF18 src/search.cpp:1250-1253 doDeeper/doShallower full-depth
            // re-search depth adjustment. When the reduced search returned
            // way above bestValue, the position is more interesting than we
            // thought — search deeper on re-search. When it barely beat
            // alpha, the move isn't that strong — search shallower.
            // Magnitudes in SF's cp: +50/-9. Hypersion 5x scale: +250/-45.
            int adjustedDepth = newDepth;
            if (!should_stop() && v > alpha && r > 0) {
                bool doDeeper    = reducedDepth < newDepth && v > bestValue + 250;
                bool doShallower = v < bestValue + 45;
                adjustedDepth += int(doDeeper) - int(doShallower);
                v = -search(pos, ss + 1, -alpha - 1, -alpha, adjustedDepth, childPv,
                            /*isPv=*/false, !cutNode);
                // 2026-05-17 finding #108: SF18:1258-1259 awards a positive
                // 2-ply continuation-history bonus when the LMR full-depth
                // re-search succeeded (v > alpha). The fact that a reduced
                // move survived re-search is a strong signal it deserves
                // earlier consideration next time.
                if (v > alpha && !isCapture
                    && prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none()) {
                    constexpr int LMR_SURVIVOR_BONUS = 1365;
                    contHist[0]->update(prevPiece1, prevMove1.to_sq(), moving, m.to_sq(),  LMR_SURVIVOR_BONUS);
                    if (prevPiece2 != NO_PIECE && prevMove2 != Move::null() && prevMove2 != Move::none())
                        contHist[1]->update(prevPiece2, prevMove2.to_sq(), moving, m.to_sq(), LMR_SURVIVOR_BONUS / 2);
                }
            }
            // If still better than alpha and we're in a PV node, full window re-search.
            if (!should_stop() && v > alpha && (isPv || v < beta))
                v = -search(pos, ss + 1, -beta, -alpha, adjustedDepth, childPv,
                            /*isPv=*/true, false);
        }

        pos.undo_move(m);

        if (should_stop()) return VALUE_ZERO;

        if (v > bestValue) {
            bestValue = v;
            if (v > alpha) {
                bestMove = m;
                if (isPv) update_pv(pv, m, childPv);
                // 2026-05-17 finding #112: SF18:1380-1381 reduces the
                // remaining-siblings depth by 2 plies once a move raised
                // alpha (but didn't beta-cut) at moderate depth. Remaining
                // siblings only need to fail low against the new (higher)
                // alpha, so less depth is enough — saves nodes.
                if (depth > 2 && depth < 14 && v < beta
                    && std::abs(v) < VALUE_TB_WIN_IN_MAX_PLY)
                    depth -= 2;
                if (v >= beta) {
                    // SF18 cutoffCnt tally: count this fail-high so that any
                    // sibling-move's child LMR (which reads (ss+1)->cutoffCnt)
                    // can use the cutoff concentration as a reduction signal.
                    // SF gates with `(extension < 2) || PvNode` to avoid
                    // double-counting double-extension paths; Hypersion's
                    // singular extension only ever produces extension <= 1
                    // so the gate is always true here -> just increment.
                    // Source: SF18 src/search.cpp:1374.
                    ++ss->cutoffCnt;
                    // ---- History updates on beta cutoff ----
                    // NOTE: tested SF18 ttMove confirmation bonus
                    //   `bonus = history_bonus(depth) + (m == ttMove ? 350 : 0)`
                    // matching SF18 src/search.cpp:1834 (`+347*(bestMove==ttMove)`,
                    // scaled for Hypersion's 0..2000 bonus cap vs SF's 0..1515).
                    // Result: 30g triage +82.6 +/- 89.9 (PROMISING) but
                    // 200g confirm -5.2 +/- 37.2 — classic PROTOCOL.md "30g
                    // fakeout" pattern. Within noise band, point estimate
                    // mildly negative.
                    // Hypothesis: over-weighting the TT signal in history at
                    // the expense of discovery. Or magnitude wrong for
                    // Hypersion's quadratic bonus shape (Hypersion bonus
                    // grows faster, so adding +350 atop a depth-9 cap of
                    // 2000 only nudges by 17 % at saturation but by 100+ %
                    // at low depth — the low-depth over-weighting may
                    // dominate). Future contributor wanting to retry should
                    // sweep magnitude (e.g. 100, 175, 250) and possibly
                    // gate on depth >= 4 (only at higher depths where TT
                    // signal is more trustworthy).
                    // NOTE: Phase 4.1 cluster-port attempt — combine SF18's
                    // linear bonus + ttMove bonus + parent-statScore feedback
                    // + progressive malus + capHist scaling all together as a
                    // coherent bundle. Result: -223.3 +/- 121.7 ELO at 30g
                    // (disastrous, well below -50 reject bar).
                    //
                    // Diagnosis: even the FULL cluster regresses badly because
                    // Hypersion lacks SF's supporting infrastructure that the
                    // cluster's tuning depends on:
                    //   - LowPlyHistory (separate table for ply < 4)
                    //   - PawnHistory (tombstoned at -49 ELO)
                    //   - 6-deep contHist (tombstoned at -24 ELO)
                    //   - Specific LMR statScore divisor calibrated to /11248
                    //     against the 5-of-6 contHist sum (not Hypersion's
                    //     /8192 against 2-deep).
                    //
                    // Future contributor wanting to retry: must FIRST add
                    // LowPlyHistory, PawnHistory, AND 6-deep contHist with
                    // their tuned divisors. Phase 4.1 'cluster' really
                    // requires the full SF history infrastructure, not just
                    // the bonus formula change.
                    int bonus = history_bonus(depth);
                    if (!isCapture) {
                        killers.update(ply, m);
                        if (prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none())
                            counterMoves.set(prevPiece1, prevMove1.to_sq(), m);
                        // Bonus to the fail-high quiet (butterfly + contHist).
                        update_quiet_history(pos, m, bonus, prevPiece1, prevMove1, prevPiece2, prevMove2);
                        // 2026-05-18 Tier 2: also update threat-square HH on
                        // the same cutoff signal. ss->threatSq computed at
                        // search() entry.
                        threatHist.update(pos.side_to_move(), ss->threatSq, m, bonus);
                        // Demote also-tried-quiets in threatHist too.
                        for (int i = 0; i < quietCount; ++i) {
                            int malus = bonus;
                            if (i > 5) malus -= malus * (i - 5) / i;
                            threatHist.update(pos.side_to_move(), ss->threatSq, quietsTried[i], -malus);
                        }
                        // 2026-05-17 audit #116: separate malus formula
                        // (history_malus, SPSA-tunable independent of bonus)
                        // + moveCount taper from SF18 src/search.cpp:1846-1849.
                        // Defaults make history_malus == history_bonus so
                        // current behavior is bit-identical until SPSA moves
                        // HIST_MALUS_CAP / HIST_MALUS_DEPTH* outward.
                        int malusBase = history_malus(depth);
                        for (int i = 0; i < quietCount; ++i) {
                            int malus = malusBase;
                            if (i > 5)
                                malus -= malus * (i - 5) / i;
                            update_quiet_history(pos, quietsTried[i], -malus,
                                                 prevPiece1, prevMove1, prevPiece2, prevMove2);
                        }
                    } else {
                        PieceType victim = type_of(pos.piece_on(m.to_sq()));
                        if (m.type_of() == MT_EN_PASSANT) victim = PAWN;
                        // NOTE: SF18 capture-history scaling re-tested 2026-
                        // 05-12 at smaller 1.13x (vs prior 1.395x at +5.2
                        // ELO borderline). Bundled with NMP changes; LTC
                        // 20g cumulative -34.9 ± 111. Reverted.
                        captureHist.update(moving, m.to_sq(), victim, bonus);
                    }
                    for (int i = 0; i < captureCount; ++i) {
                        Move cm = capturesTried[i];
                        Piece cmp = pos.piece_on(cm.from_sq());
                        PieceType victim = type_of(pos.piece_on(cm.to_sq()));
                        if (cm.type_of() == MT_EN_PASSANT) victim = PAWN;
                        captureHist.update(cmp, cm.to_sq(), victim, -bonus);
                    }
                    break;
                }
                alpha = v;
            }
        }

        if (!isCapture && quietCount < 128) quietsTried[quietCount++] = m;
        if ( isCapture && captureCount < 64) capturesTried[captureCount++] = m;
    }

    if (moveCount == 0) {
        return ss->excludedMove != Move::none() ? alpha
             : inCheck                          ? mated_in(ply)
                                                : VALUE_DRAW;
    }

    // 2026-05-17 audit #125: fail-low prior-opponent-quiet bonus.
    // When this node fails low and the parent's move (at ss-1) was a quiet,
    // award it a positive butterfly + contHist signal — the parent's move
    // is "doing well" from the opponent's perspective, so it should be
    // ordered higher when reached from the same parent position later.
    // SF18 src/search.cpp:1424-1445. Magnitude SHIPPED WITHOUT SPRT.
    // SF18's #124 (alpha-raise-no-cutoff history update) and #126 (prior-
    // capture fail-low bonus) require deeper structural changes — moving
    // the cutoff-internal history updates to post-loop, and tracking the
    // piece-captured-by-prior-move. Both still deferred.
    if (bestValue <= alpha && bestMove == Move::none() && ply > 0
        && prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none()
        && !pos.captured_piece()) {
        int priorBonus = history_bonus(depth) / 2;
        // Increase opponent's mainHist for their parent move.
        mainHist.update(~pos.side_to_move(), prevMove1, priorBonus);
        // Increase contHist projection from grandparent move -> parent move.
        if (prevPiece2 != NO_PIECE && prevMove2 != Move::null() && prevMove2 != Move::none())
            contHist[0]->update(prevPiece2, prevMove2.to_sq(),
                                prevPiece1, prevMove1.to_sq(), priorBonus);
    }

    // 2026-05-17 finding #131: SF18:1460-1461 bestows ttPv from parent on
    // fail-low. Lets a "this branch has ever been PV" signal propagate up
    // even when the current node failed low (so descendants of the same
    // parent get the sticky less-aggressive pruning treatment).
    if (bestValue <= alpha && ply >= 1)
        ss->ttPv = ss->ttPv || (ss - 1)->ttPv;

    // 2026-05-17 finding #129: SF18:1407-1408 moderates fail-high bestValue
    // toward beta before TT-storing. Pulls over-shoot fail-high scores back
    // so TT readers don't see inflated lower bounds. Gated on non-decisive
    // values so TB-win / mate scores still propagate cleanly.
    if (bestValue >= beta
        && std::abs(bestValue) < VALUE_TB_WIN_IN_MAX_PLY
        && std::abs(alpha)     < VALUE_TB_WIN_IN_MAX_PLY)
        bestValue = Value((bestValue * depth + beta) / (depth + 1));

    // 2026-05-17 audit #19/#130: clamp bestValue against TB UPPER-bound hint
    // BEFORE bound/TT-write so over-estimated tactical fail-highs can't
    // overwrite the proven TB result. PvNode only — non-PV paths returned
    // immediately if the cutoff fired.
    if (isPv && bestValue > maxValue)
        bestValue = maxValue;

    Bound b = bestValue >= beta            ? BOUND_LOWER
            : (isPv && bestMove != Move::none()) ? BOUND_EXACT : BOUND_UPPER;
    // 2026-05-17 finding #132: SF18:1465 guards TT write with !excludedMove.
    // Inside SE recursion (excludedMove != none) the inner search uses the
    // same posKey as the outer call; writing here corrupts the entry the
    // outer node will read after the SE test.
    if (ss->excludedMove == Move::none())
        tte->save(pos.key(), TT.value_to_tt(bestValue, ply), ttPv, b, depth,
                  bestMove, rawEval, TT.generation());

    // ---- Correction history update ----
    // When the actual search outcome disagrees with the static eval, nudge the
    // pawn-key bucket toward the difference.
    //
    // 2026-05-17 audit #136: SF18 src/search.cpp:1475-1481 gates on
    // `(bestValue > staticEval) == bool(bestMove)`. Logic:
    //   - bestMove found: only update when bestValue > staticEval (search
    //     genuinely improved on static eval — corrhist should pull eval UP)
    //   - no bestMove (fail-low): only update when bestValue <= staticEval
    //     (search confirmed static eval was too optimistic — corrhist should
    //     pull eval DOWN)
    // Hypersion previously skipped fail-low updates entirely. Including them
    // doubles the corrhist signal volume. Capture bestMoves are still excluded
    // because their value comes from tactical exchange, not positional eval.
    // SHIPPED WITHOUT SPRT.
    bool capturedBest = bestMove != Move::none() && pos.capture(bestMove);
    if (!inCheck && rawEval != VALUE_NONE
        && !capturedBest
        && (bestValue > rawEval) == (bestMove != Move::none())) {
        int diff = int(bestValue - rawEval) * 256;
        int weight = std::min(64, depth * 4 + 8);
        pawnCorrHist.update(pos.side_to_move(), pos.pawn_key(), diff, weight);
        // 2026-05-18 Tier 1: ALSO update contCorrHist1 + contCorrHist2 on
        // the same (real - raw) signal. Two table writes per qualifying
        // search step. Keys mirror the read-side: outer = (ss-2) or
        // (ss-3)'s piece+to, inner = (ss-1)'s piece+to.
        if (ply >= 2 && (ss - 1)->currentMove != Move::none()
                     && (ss - 1)->currentMove != Move::null()
                     && (ss - 1)->movedPiece != NO_PIECE) {
            Piece innerPc = (ss - 1)->movedPiece;
            Square innerTo = (ss - 1)->currentMove.to_sq();
            if (ply >= 3 && (ss - 2)->currentMove != Move::none()
                         && (ss - 2)->currentMove != Move::null()
                         && (ss - 2)->movedPiece != NO_PIECE) {
                contCorrHist2.update((ss - 2)->movedPiece,
                                     (ss - 2)->currentMove.to_sq(),
                                     innerPc, innerTo, diff, weight);
            }
            if (ply >= 4 && (ss - 3)->currentMove != Move::none()
                         && (ss - 3)->currentMove != Move::null()
                         && (ss - 3)->movedPiece != NO_PIECE) {
                contCorrHist1.update((ss - 3)->movedPiece,
                                     (ss - 3)->currentMove.to_sq(),
                                     innerPc, innerTo, diff, weight);
            }
        }
    }
    return bestValue;
}

// ---------------------------------------------------------------------------
// UCI info
// ---------------------------------------------------------------------------
// score_to_wdl — convert a search score into approximate win/draw/loss
// probabilities (per-mille, summing to 1000). The model is a simple
// double-sigmoid calibrated to NNUE-cp magnitudes (Hypersion 2 leaves
// raw NNUE values without an attenuation divisor, so a half-pawn ≈ 60 cp
// roughly maps to a 70/30 win expectation).
//   Mate scores → all-or-nothing (1000 / 0 / 0 or 0 / 0 / 1000).
static void score_to_wdl(Value score, int& w, int& d, int& l) {
    if (score >= VALUE_MATE_IN_MAX_PLY)  { w = 1000; d = 0; l = 0; return; }
    if (score <= -VALUE_MATE_IN_MAX_PLY) { w = 0; d = 0; l = 1000; return; }
    // Sigmoid scale picked so that ±300 cp ≈ 90/10 split.
    double cp = double(int(score));
    double scale = 130.0;
    double offset = 50.0;             // cp ≈ 50 needed for 50/50 → "advantage" line
    double w_p = 1.0 / (1.0 + std::exp(-(cp - offset) / scale));
    double l_p = 1.0 / (1.0 + std::exp((cp + offset) / scale));
    double d_p = 1.0 - w_p - l_p;
    if (d_p < 0) d_p = 0;
    w = int(w_p * 1000.0 + 0.5);
    d = int(d_p * 1000.0 + 0.5);
    l = 1000 - w - d;
    if (l < 0) { l = 0; d = std::max(0, 1000 - w); }
}

void Worker::print_info(int depth, int selDepthVal, Value score,
                        std::uint64_t totalNodes, TimePoint elapsed, const PVLine& pv,
                        int pvIdx, int multiPv, int boundFlag) {
    int64_t ms = std::max<int64_t>(elapsed, 1);
    std::uint64_t nps = (totalNodes * 1000) / std::uint64_t(ms);

    std::cout << "info depth " << depth
              << " seldepth " << std::max(depth, selDepthVal);
    if (multiPv > 1) std::cout << " multipv " << (pvIdx + 1);
    std::cout << " score " << score_to_uci(score);
    if      (boundFlag == 1) std::cout << " upperbound";
    else if (boundFlag == 2) std::cout << " lowerbound";
    if (limits.showWDL) {
        int w, d, l;
        score_to_wdl(score, w, d, l);
        std::cout << " wdl " << w << ' ' << d << ' ' << l;
    }
    std::cout << " nodes " << totalNodes
              << " nps "   << nps
              << " hashfull " << TT.hashfull()
              << " time "  << ms
              << " pv";
    for (int i = 0; i < pv.length; ++i) std::cout << ' ' << move_uci(pv.moves[i]);
    std::cout << std::endl;
}

}  // namespace hypersion::Search
