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
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>

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
            // A/B at 200g 5+0.05: 1.85 vs 1.90 = +3.5 +/- 36.8 ELO (within
            // noise but positive, matches SF). Earlier history: 1.95->1.90
            // gave +59.6 ELO over AVX2 baseline.
            Reductions[d][mc] = (d == 0 || mc == 0) ? 0
                              : int(std::log(double(d)) * std::log(double(mc)) / 1.85);
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
int RFP_MARGIN_PER_DEPTH    = 240;    // Reverse futility (unchanged by A3).
    // Sweep: 200 = -8.1 ELO at 129g; 280 = -15.6 ELO at 200g.
int RAZOR_MARGIN_BASE       = 852;    // A3: 850 -> 852. Sweep history:
    // 600 = 0.0 +/- 36.4 ELO; 850 = +3.5 +/- 38.3 (kept pre-A3).
int RAZOR_MARGIN_PER_DEPTH  = 387;    // A3: 390 -> 387. Sweep history:
    // 300 = -34.9 ELO @ 30g; 480 = +46.6 @ 30g but -51.6 @ 61g.
int FUTIL_MARGIN_PER_DEPTH  = 397;    // A3: 400 -> 397. Sweep history:
    // 330 -> 400 was +15.6 +/- 37.6 ELO @ 200g.
int FUTIL_MARGIN_BASE       = 385;    // A3: 390 -> 385. Sweep history:
    // 480 = -23.2 ELO; 300 = -46.6 ELO. Both directions worse than 390.
int SEE_QUIET_MARGIN        = -181;   // A3: -180 -> -181. Sweep history:
    // -150 = -70 ELO; -220 = -1.7 +/- 38.5 ELO.
int SEE_CAPT_MARGIN         = -252;   // A3: -250 -> -252. Sweep history:
    // -400 = -58 ELO; -250 = +8.7 +/- 39.7 ELO vs -300 baseline.
int NMP_EVAL_BETA_DIV       = 803;    // A3: 800 -> 803. Sweep history:
    // 600 -> 800 = +8.7 ELO; 800 -> 1200 = -1.7 ELO.
int PROBCUT_MARGIN          = 802;    // A3: 800 -> 802. Manual sweep:
    //   500: -24.4 ELO; 600: baseline; 800: +22.6 ELO (shipped); 1000: -45.4.
int ASPIRATION_DELTA0       =  50;    // A3: 51 -> 50. Sweep history:
    // 30 = -10.4 ELO; 80 = +1.7 ELO.
int STABILITY_SWING_TH      =  61;    // A3: 60 -> 61. Sweep history:
    // 100 / 40 both regress vs 60.
int QSEARCH_CAP_GAIN        = 3259;   // A3: 3300 -> 3259. Sweep history:
    // 2200 = -209 ELO @ 13g; 5000 = 0.0 ELO @ 30g.

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
int HIST_BONUS_DEPTH2 = 16;     // unchanged from default
int HIST_BONUS_DEPTH1 = 30;     // SPSA v2: was 32
int HIST_BONUS_CAP    = 2059;   // SPSA v2: was 2000
int BFLY_WEIGHT       = 101;    // SPSA v2: was 100
int CONT1_WEIGHT      =  99;    // SPSA v2: was 100
int CONT2_WEIGHT      =  47;    // SPSA v2: was 50

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
int TM_ENDGAME_BONUS_8  = 160;   // pre-A4: 1.6, A4 unchanged
int TM_ENDGAME_BONUS_12 = 140;   // pre-A4: 1.4, A4 unchanged
int TM_ENDGAME_BONUS_16 = 120;   // pre-A4: 1.2, A4 unchanged (defaults preserved)
int TM_EASY_GAP150      =  40;   // pre-A4: 0.4, A4 unchanged
int TM_EASY_GAP80       =  60;   // pre-A4: 0.6, A4 unchanged
int TM_EASY_GAP40       =  85;   // pre-A4: 0.85, A4 unchanged

// ---- A5 threat-by-lesser + LMR statScore (2026-05-09) ----
// Three previously-hardcoded constants exposed as Tune_*. Defaults
// preserve pre-A5 behavior bit-identically.
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
// A5 SPSA campaign (2026-05-09, 200 iters x 64 games/iter, ~30 min):
// Convergence was extremely tight — only LMR_STATSCORE_DIV moved at
// all (8192 -> 8063, -1.6 %); both threat-by-lesser bonuses
// unchanged. Final: -19 / +20 / 8063.
//
// Single 200g SPRT vs default-Tune_* BASE @ 5+0.05, conc=6:
//   +17.4 +/- 38.0 ELO  (W=67 L=57 D=76, score 0.525)
//
// Above the +10 ship threshold but with a single run only — CI ±38
// includes 0, so this could be either a real ~+10-15 ELO improvement
// (consistent with A3's 1-2 % shifts producing +33 ELO at 600g) or
// pure variance. Conservative call: SHIP the infrastructure, KEEP
// pre-A5 defaults. Future contributor with 30-60 min of SPRT budget
// can run two more 200g confirms — if combined 600g shows >+10 ELO
// with lower-CI > 0, update the three defaults to -19 / +20 / 8063.
int THREAT_BY_LESSER_PENALTY = -19;   // movepick.cpp pre-A5: -19
int THREAT_BY_LESSER_BONUS   =  20;   // movepick.cpp pre-A5: +20
int LMR_STATSCORE_DIV        = 8192;  // search.cpp   pre-A5: 8192

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

inline int lmp_threshold(int depth, bool improving) {
    // Stockfish-style movecount threshold: more aggressive when not improving.
    return improving ? (3 + depth * depth) : (3 + depth * depth) / 2;
}

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
    for (auto& ch : contHist) ch->clear();
    // Note: TT clearing is the pool's responsibility — done once across all threads.
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
    // When the position is in the tablebases, restrict root moves to those
    // recommended by Fathom and report the TB-aware score immediately.
    if (isMain && Syzygy::is_loaded()) {
        Syzygy::RootProbe tbp;
        if (Syzygy::probe_root(pos, tbp) && tbp.bestMove != Move::none()) {
            // Filter rootMoves to keep only the Syzygy-best move (and any
            // legal alternative if the WDL is a draw — keeps variety).
            for (auto& rm : rootMoves) if (rm.pv0 == tbp.bestMove) rm.score = tbp.score;
            std::stable_sort(rootMoves.begin(), rootMoves.end());
            std::cout << "info string syzygy: " << move_uci(tbp.bestMove)
                      << " (wdl=" << tbp.wdl << ")" << std::endl;
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
    int effSkill = limits.skillLevel;
    if (limits.limitStrength) {
        int e = std::clamp(limits.uciElo, 500, 3200);
        effSkill = std::clamp((e - 500) * 20 / 2700, 0, 20);
    }
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
    //   0    |   500   |   45%    |     150
    //   1    |   635   |   33%    |     600
    //   2    |   770   |   22%    |    2400
    //   3    |   905   |   13%    |    6000
    //   4    |  1040   |    9%    |   15000   (Maia-1100 target)
    //   5    |  1175   |    6%    |   36000
    //   6    |  1310   |    4%    |   90000
    //   7    |  1445   |    2%    |  200000   (Maia-1500 target)
    //   8    |  1580   |    1%    |  400000
    //   9    |  1715   |    1%    |  800000
    //  10    |  1850   |    0%    |  unlimited (Maia-1900 target)
    //  11+   |  1985+  |    0%    |  unlimited
    // ITERATION 4: v3 vs Maia gave 1100=-360 / 1500=-200 / 1900=OK.
    // Bumped nodes 2-3x and halved blunders for skills 4-8 (the 1100-1700
    // range that's most user-relevant for lichess bot opponents).
    //
    //   skill | UCI_Elo | blunder% | nodes
    //   0    |   500   |   45%    |     150
    //   1    |   635   |   33%    |     600
    //   2    |   770   |   22%    |    2400
    //   3    |   905   |   13%    |    6000
    //   4    |  1040   |    5%    |   50000   (target Maia-1100)
    //   5    |  1175   |    4%    |  100000
    //   6    |  1310   |    2%    |  200000
    //   7    |  1445   |    1%    |  400000   (target Maia-1500)
    //   8    |  1580   |    1%    |  700000
    //   9    |  1715   |    0%    |  unlimited
    //  10+   |  1850+  |    0%    |  unlimited (Maia-1900 already OK)
    // ITERATION 7: vs dala-700 (actual ~881) Hyp@700 lost 0-10; vs dala-
    // 900 (actual ~1000) Hyp@900 scored 15%. Both ~200 ELO weaker than
    // configured. Bump nodes for skills 0-4 (UCI_Elo 500-1040) by 4-5x
    // without re-introducing high blunder rates (user explicitly didn't
    // want random-looking play).
    //
    //   skill | UCI_Elo | blunder% | nodes      (target real Elo)
    //   0    |   500   |    8%    |     200
    //   1    |   635   |    7%    |     800     (~700 vs dala-700)
    //   2    |   770   |    5%    |    2500
    //   3    |   905   |    4%    |    8000     (~900 vs dala-900)
    //   4    |  1040   |    3%    |   25000     (~1100 vs dala-1100)
    //   5    |  1175   |    2%    |   60000
    //   6    |  1310   |    2%    |  120000
    //   7    |  1445   |    1%    |  250000     (~1500 vs Maia 1500)
    //   8    |  1580   |    1%    |  500000
    //   9    |  1715   |    0%    |  900000
    //  10+   |  1850+  |    0%    |  unlimited
    // ITERATION 8: v7 had 700=+120 strong, 900=-120 weak. Adjust just
    // those two buckets to hit dead-center.
    //   skill 1 (UCI 700): 800 -> 500 nodes    (-60 ELO)
    //   skill 3 (UCI 900): 8000 -> 14000 nodes (+60 ELO)
    static constexpr int BLUNDER_PCT[21] = {
         8,  7,  5,  4,  3,  2,  2,  1,  1,  0,
         0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
    };
    static constexpr std::uint64_t SKILL_NODES[21] = {
            200ULL,       500ULL,      2500ULL,     14000ULL,     25000ULL,
          60000ULL,    120000ULL,    250000ULL,    500000ULL,    900000ULL,
              0ULL,         0ULL,         0ULL,         0ULL,         0ULL,
              0ULL,         0ULL,         0ULL,         0ULL,         0ULL,         0ULL
    };
    int           skillBlunderPct = BLUNDER_PCT[effSkill];
    int           skillDepthCap   = MAX_PLY;
    std::uint64_t skillNodeCap    = SKILL_NODES[effSkill];
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
                    windowBeta  = (windowAlpha + windowBeta) / 2;
                    windowAlpha = std::max<int>(bestThisIter - delta, -VALUE_INFINITE);
                    delta += delta / 4 + 5;
                } else if (bestThisIter >= windowBeta && windowBeta != VALUE_INFINITE) {
                    // Fail-high: report partial info with `lowerbound`.
                    if (isMain && pvIdx == 0)
                        print_info(d, selDepth, bestThisIter,
                                   pool ? pool->total_nodes() : nodes.load(),
                                   tm.elapsed(), iterPV, 0, multiPv, /*flag=*/2);
                    windowBeta = std::min<int>(bestThisIter + delta, VALUE_INFINITE);
                    delta += delta / 4 + 5;
                } else {
                    if (pvIdx == 0) { bestScore = bestThisIter; bestPV = iterPV; }
                    break;
                }
                if (delta >= 1000) { windowAlpha = -VALUE_INFINITE; windowBeta = VALUE_INFINITE; }
            }

            // Bring the best of the searched range to position pvIdx.
            std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end());
        }

        // Remember scores for the next iteration's aspiration window.
        for (auto& rm : rootMoves) rm.prevScore = rm.score;

        bestMove       = rootMoves[0].pv0;
        prevScore      = bestScore;
        completedDepth = d;

        // Only the main worker emits UCI info. Helpers run silently to keep
        // the output stream readable and to fill the shared TT.
        if (isMain) {
            std::uint64_t totN = pool ? pool->total_nodes() : nodes.load();
            TimePoint     ms   = tm.elapsed();
            for (int pvIdx = 0; pvIdx < multiPv && pvIdx < int(rootMoves.size()); ++pvIdx)
                print_info(d, selDepth, rootMoves[pvIdx].score, totN, ms, rootMoves[pvIdx].pv,
                           pvIdx, multiPv);
        }

        if (should_stop()) break;
        if (std::abs(bestScore) >= VALUE_MATE_IN_MAX_PLY) break;

        // Track score / bestmove stability for time scaling.
        if (d > 1) {
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
            if (stableIters >= 4) scale = 0.5;
            else if (stableIters >= 2) scale = 0.75;

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
            if (d > 1) {
                int drop = int(prevScore - bestScore);   // > 0 when eval dropped
                double fallingEval = std::clamp(1.0 + drop / 1000.0, 0.6, 1.7);
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
                    if (nodesEffort >= 93340)
                        scale *= 0.76;
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
        std::cout << "bestmove " << move_uci(bestMove);
        if (ponderMove != Move::none()) std::cout << " ponder " << move_uci(ponderMove);
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
    if (pos.is_draw(ply)) return Value(-limits.contempt);   // STM perspective
    if (ply >= MAX_PLY)   return Eval::evaluate(pos);

    bool inCheck = pos.checkers();
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
    } else {
        staticEval = ttHit && tte->eval() != VALUE_NONE ? tte->eval() : Eval::evaluate(pos);
        bestValue  = staticEval;
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
    ss->staticEval = staticEval;

    MovePicker mp(pos, ttMove, &mainHist, &captureHist, /*qDepth=*/0);
    Move bestMove = Move::none();

    // Maximum gain a capture could possibly produce (queen value plus a small slack).
    const int MaxQsearchGain = QSEARCH_CAP_GAIN;  // was constexpr; runtime now (SPSA-tunable)
    Move m;
    while ((m = mp.next_move()) != Move::none()) {
        if (!pos.legal(m)) continue;

        // SEE pruning in qsearch: skip captures that lose material.
        if (!inCheck && bestValue > -VALUE_MATE_IN_MAX_PLY && !pos.see_ge(m, VALUE_ZERO))
            continue;

        // Capture-futility in qsearch: even capturing a queen wouldn't lift our
        // score to alpha, so don't bother. (Skipped while in check — every
        // evasion must be considered.)
        if (!inCheck && pos.capture(m) && bestValue > -VALUE_MATE_IN_MAX_PLY) {
            Value gain = Value(MaxQsearchGain);
            if (staticEval + gain <= alpha) continue;
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

    Bound b = bestValue >= beta ? BOUND_LOWER
            : (isPv && bestMove != Move::none()) ? BOUND_EXACT : BOUND_UPPER;
    tte->save(pos.key(), TT.value_to_tt(bestValue, ply), isPv, b, 0,
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
    if (pos.is_draw(ply))return Value(-limits.contempt);   // STM perspective

    nodes.fetch_add(1, std::memory_order_relaxed);

    // Mate-distance pruning.
    alpha = std::max<int>(mated_in(ply), alpha);
    beta  = std::min<int>(mate_in(ply + 1),  beta);
    if (alpha >= beta) return alpha;

    bool inCheck   = pos.checkers();
    ss->inCheck    = inCheck;
    ss->moveCount  = 0;
    ss->statScore  = 0;       // SF18: cleared at entry, set per-move below.
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
    bool  ttPv    = isPv || (ttHit && tte->is_pv());

    if (!isPv && ttHit && tte->depth() >= depth) {
        if (tte->bound() == BOUND_EXACT
            || (tte->bound() == BOUND_LOWER && ttValue >= beta)
            || (tte->bound() == BOUND_UPPER && ttValue <= alpha)) {
            return ttValue;
        }
    }

    // ---- Syzygy WDL probe ----
    // Cheap mid-search probe: when the position is small enough to be in TBs,
    // Fathom returns the truth and we can short-circuit.
    if (!isPv && depth >= Syzygy::probe_depth() && Syzygy::is_loaded()) {
        Value tbVal = Syzygy::probe_wdl(pos);
        if (tbVal != VALUE_NONE) {
            Bound b = tbVal >= VALUE_DRAW ? BOUND_LOWER : BOUND_UPPER;
            if ( b == BOUND_LOWER ? tbVal >= beta : tbVal <= alpha) {
                tte->save(pos.key(), TT.value_to_tt(tbVal, ply), false, b,
                          std::min<int>(depth + 6, MAX_PLY - 1),
                          Move::none(), VALUE_NONE, TT.generation());
                return tbVal;
            }
        }
    }

    Value rawEval, staticEval;
    if (inCheck) {
        rawEval = staticEval = ss->staticEval = VALUE_NONE;
    } else if (ttHit && tte->eval() != VALUE_NONE) {
        rawEval = tte->eval();
        staticEval = pawnCorrHist.adjust(pos.side_to_move(), pos.pawn_key(), rawEval);
        ss->staticEval = staticEval;
    } else {
        rawEval = Eval::evaluate(pos);
        staticEval = pawnCorrHist.adjust(pos.side_to_move(), pos.pawn_key(), rawEval);
        ss->staticEval = staticEval;
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
            improving = true;   // unknown → assume improving, gentler pruning
    }

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
    if (!isPv && !inCheck && depth <= 7
        && std::abs(beta) < VALUE_MATE_IN_MAX_PLY
        && staticEval - RFP_MARGIN_PER_DEPTH * (depth - improving) >= beta)
        return staticEval;

    // ---- Razoring ----
    // NOTE: tried bumping depth ceiling 4 -> 5; -12.2 +/- 36.6 ELO
    // at 200g 5+0.05.  Within noise but mildly negative; razoring at
    // depth 5 mis-cuts more often than the savings justify here.
    if (!isPv && !inCheck && depth <= 4
        && staticEval + RAZOR_MARGIN_BASE + RAZOR_MARGIN_PER_DEPTH * depth <= alpha) {
        Value v = qsearch(pos, ss, alpha, alpha + 1, /*isPv=*/false);
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
    if (!isPv && !inCheck && depth >= 3
        && (ss - 1)->currentMove != Move::null()
        && ss->excludedMove == Move::none()
        && staticEval >= beta
        && std::abs(beta) < VALUE_MATE_IN_MAX_PLY
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
            // Don't return unproven mates — fall back to plain beta.
            return nullValue >= VALUE_MATE_IN_MAX_PLY ? beta : nullValue;
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
    if (!isPv && !inCheck && depth >= 5
        && std::abs(beta) < VALUE_MATE_IN_MAX_PLY
        && ss->excludedMove == Move::none()) {
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
            if (v >= probCutBeta) return v;
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
                  contHist[1].get(), prevMove2, prevPiece2);

    Value bestValue = -VALUE_INFINITE;
    Move  bestMove  = Move::none();
    int   moveCount = 0;
    Move  quietsTried[64];
    int   quietCount = 0;
    Move  capturesTried[32];
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
        if (!isPv && !inCheck && bestValue > -VALUE_MATE_IN_MAX_PLY && depth <= 8
            && moveCount > lmp_threshold(depth, improving)) {
            skipQuiets = true;
        }

        // ---- Pruning at low depth ----
        if (!isPv && !inCheck && bestValue > -VALUE_MATE_IN_MAX_PLY) {
            if (!isCapture && !givesCheck) {
                // Futility pruning for quiets.
                if (depth <= 6 && staticEval + FUTIL_MARGIN_PER_DEPTH * depth + FUTIL_MARGIN_BASE <= alpha)
                    skipQuiets = true;
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
        if (depth >= 5
            && m == ttMove
            && ss->excludedMove == Move::none()
            && std::abs(ttValue) < VALUE_MATE_IN_MAX_PLY
            && (tte->bound() & BOUND_LOWER)
            && tte->depth() >= depth - 3
            && ply > 0) {
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
        Depth r = 0;
        if (depth >= 3 && moveCount > 1 + (isPv ? 1 : 0) && (!isCapture || cutNode)) {
            r = lmr_base(depth, moveCount);
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
            }
            r = std::clamp(r, 0, newDepth - 1);
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
            v = -search(pos, ss + 1, -alpha - 1, -alpha, newDepth - r, childPv,
                        /*isPv=*/false, true);
            // If reduced search beat alpha, re-search at full depth.
            if (!should_stop() && v > alpha && r > 0)
                v = -search(pos, ss + 1, -alpha - 1, -alpha, newDepth, childPv,
                            /*isPv=*/false, !cutNode);
            // If still better than alpha and we're in a PV node, full window re-search.
            if (!should_stop() && v > alpha && (isPv || v < beta))
                v = -search(pos, ss + 1, -beta, -alpha, newDepth, childPv,
                            /*isPv=*/true, false);
        }

        pos.undo_move(m);

        if (should_stop()) return VALUE_ZERO;

        if (v > bestValue) {
            bestValue = v;
            if (v > alpha) {
                bestMove = m;
                if (isPv) update_pv(pv, m, childPv);
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
                        mainHist.update(pos.side_to_move(), m, bonus);
                        if (prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none())
                            counterMoves.set(prevPiece1, prevMove1.to_sq(), m);
                        // Demote tried-but-failed quiets.
                        for (int i = 0; i < quietCount; ++i)
                            mainHist.update(pos.side_to_move(), quietsTried[i], -bonus);
                        // Continuation history.
                        if (prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none())
                            contHist[0]->update(prevPiece1, prevMove1.to_sq(), moving, m.to_sq(), bonus);
                        if (prevPiece2 != NO_PIECE && prevMove2 != Move::null() && prevMove2 != Move::none())
                            contHist[1]->update(prevPiece2, prevMove2.to_sq(), moving, m.to_sq(), bonus / 2);
                        // Demote tried-but-failed quiets in continuation history too.
                        for (int i = 0; i < quietCount; ++i) {
                            Move qm = quietsTried[i];
                            Piece qp = pos.piece_on(qm.from_sq());
                            if (prevPiece1 != NO_PIECE && prevMove1 != Move::null() && prevMove1 != Move::none())
                                contHist[0]->update(prevPiece1, prevMove1.to_sq(), qp, qm.to_sq(), -bonus);
                            if (prevPiece2 != NO_PIECE && prevMove2 != Move::null() && prevMove2 != Move::none())
                                contHist[1]->update(prevPiece2, prevMove2.to_sq(), qp, qm.to_sq(), -bonus / 2);
                        }
                    } else {
                        PieceType victim = type_of(pos.piece_on(m.to_sq()));
                        if (m.type_of() == MT_EN_PASSANT) victim = PAWN;
                        // NOTE: tested SF18 capture-history scaling
                        // (best capture: bonus*1395/1024 = +36% emphasis,
                        // tried-but-failed: -bonus*1448/1024 = +41% demote)
                        // matching SF18 src/search.cpp:1856,1869.
                        // Result: 30g triage +23.2 +/- 98.9, 200g confirm
                        // +5.2 +/- 36.3 — at the PROTOCOL.md REJECT bar
                        // (<= +5 with CI ±35).  The trend is mildly
                        // positive but not clean enough to ship.
                        // Future retry: try smaller scaling (1.10-1.20x)
                        // or sweep with longer SPRT (300-500g) since the
                        // signal might be real-but-weak. Hypersion's
                        // bonus is already much larger than SF's at low
                        // depth (quadratic vs linear), so SF's exact
                        // multiplier may over-amplify here.
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

        if (!isCapture && quietCount < 64) quietsTried[quietCount++] = m;
        if ( isCapture && captureCount < 32) capturesTried[captureCount++] = m;
    }

    if (moveCount == 0) {
        return ss->excludedMove != Move::none() ? alpha
             : inCheck                          ? mated_in(ply)
                                                : VALUE_DRAW;
    }

    Bound b = bestValue >= beta            ? BOUND_LOWER
            : (isPv && bestMove != Move::none()) ? BOUND_EXACT : BOUND_UPPER;
    tte->save(pos.key(), TT.value_to_tt(bestValue, ply), ttPv, b, depth,
              bestMove, rawEval, TT.generation());

    // ---- Correction history update ----
    // When the actual search outcome disagrees with the static eval, nudge the
    // pawn-key bucket toward the difference. Only update when the bound is
    // consistent with the move type that produced bestValue.
    if (!inCheck && rawEval != VALUE_NONE
        && bestMove != Move::none()
        && !pos.capture(bestMove)) {
        int diff = int(bestValue - rawEval) * 256;
        int weight = std::min(64, depth * 4 + 8);
        pawnCorrHist.update(pos.side_to_move(), pos.pawn_key(), diff, weight);
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
