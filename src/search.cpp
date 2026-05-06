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
constexpr int RFP_MARGIN_PER_DEPTH    = 240;    // Reverse futility (was 80).
    // Sweep: 200 = -8.1 ELO at 129g (incomplete, trending negative);
    // 280 = -15.6 ELO at 200g.  Kept at 240.
constexpr int RAZOR_MARGIN_BASE       = 850;    // Razoring base.
    // Sweep vs BASE (720): 600 = 0.0 +/- 36.4 ELO; 850 = +3.5 +/- 38.3.
    // Both within noise but 850 trended positive.
constexpr int RAZOR_MARGIN_PER_DEPTH  = 390;    // (was 130)
constexpr int FUTIL_MARGIN_PER_DEPTH  = 400;    // Futility for quiets.
    // 330 -> 400 tested at +15.6 +/- 37.6 ELO @ 200g. Original 330
    // was too aggressive — futility was over-pruning quiet moves
    // that had real follow-through.
constexpr int FUTIL_MARGIN_BASE       = 390;    // (was 130; previously inline)
constexpr int SEE_QUIET_MARGIN        = -180;   // SEE pruning of bad quiets.
    // Sweep: -150 = -70 ELO @ 30g (clear regression), -220 = -1.7 +/- 38.5
    // ELO @ 200g (within noise). Kept at -180.
constexpr int SEE_CAPT_MARGIN         = -250;   // SEE pruning of bad captures.
    // Sweep vs -300: -400 = -58 ELO @ 30g (bad), -250 = +8.7 +/- 39.7
    // ELO @ 200g (positive). -300 was too aggressive.
constexpr int NMP_EVAL_BETA_DIV       = 800;    // NMP reduction-bonus divisor.
    // Sweep: 600 (was) -> 800 = +8.7 ELO; 800 -> 1200 = -1.7 ELO.
    // 800 is the sweet spot — kept.
constexpr int PROBCUT_MARGIN          = 800;    // Optimum from manual sweep:
    //   500: -24.4 ELO (too aggressive)
    //   600: baseline
    //   800: +22.6 ELO (sweet spot, shipped)
    //   1000: -45.4 ELO vs 800 (too conservative — under-prunes)
constexpr int ASPIRATION_DELTA0       = 51;     // initial aspiration delta.
    // Tested 30: -10.4 ELO (too tight), 80: +1.7 ELO (within noise).
    // Kept at 51.
constexpr int STABILITY_SWING_TH      = 60;     // bestScore swing for "stable" (was 20)
constexpr int QSEARCH_CAP_GAIN        = 3300;   // qsearch capture-futility cap (was 1100)

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

        // Soft-stop: scale optimum budget by stability. Stable for 4+ iterations
        // → take only ~50 % of optimum; volatile (recent move changes) → 1.4 ×.
        if (isMain && !limits.infinite && limits.depth == 0) {
            double scale = 1.0;
            if (stableIters >= 4) scale = 0.5;
            else if (stableIters >= 2) scale = 0.75;
            if (bestMoveChanges >= 3 && d <= 12) scale *= 1.4;

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
            if (totalPieces <= 8)        scale *= 1.6;   // K+R+P-ish endgames
            else if (totalPieces <= 12)  scale *= 1.4;   // light endgame
            else if (totalPieces <= 16)  scale *= 1.2;   // late middlegame transition

            // Phase 5: easy-move detection. When the best root move is
            // clearly better than the 2nd-best AND has been stable, we don't
            // need to keep thinking. NNUE-aware thresholds (~/3 divisor puts
            // avg eval at ~220 cp). Uses `min` so this only ever saves time
            // — never extends past the existing scale.
            if (d >= 6 && rootMoves.size() >= 2 && stableIters >= 3) {
                int gap = int(rootMoves[0].score - rootMoves[1].score);
                if (gap >= 150)      scale = std::min(scale, 0.4);
                else if (gap >= 80)  scale = std::min(scale, 0.6);
                else if (gap >= 40)  scale = std::min(scale, 0.85);
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

            // NOTE: tried SF18 `bestMoveInstability` factor here:
            //   scale *= 1.02 + 2.14 * bestMoveChanges / threads.size()
            // Result: -111 ELO at 200 games (5+0.05).
            // Root cause: Hypersion's `bestMoveChanges` is CUMULATIVE
            // across iterations (only ever increments), while SF's
            // `totBestMoveChanges` resets/decays per iteration. With a
            // cumulative counter on a single-threaded run, the multiplier
            // explodes (10×+), gobbling the whole clock. To safely port
            // this, Hypersion would need to track per-iteration changes
            // separately — left as future work.

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
        if (bestValue >= beta)  { return bestValue; }
        if (bestValue >  alpha) alpha = bestValue;
    }
    ss->staticEval = staticEval;

    MovePicker mp(pos, ttMove, &mainHist, &captureHist, /*qDepth=*/0);
    Move bestMove = Move::none();

    // Maximum gain a capture could possibly produce (queen value plus a small slack).
    constexpr int MaxQsearchGain = QSEARCH_CAP_GAIN;
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

    // ---- TT probe ----
    bool ttHit = false;
    TTEntry* tte = TT.probe(pos.key(), ttHit);
    Value ttValue = ttHit ? TT.value_from_tt(tte->value(), ply, pos.rule50_count()) : VALUE_NONE;
    Move  ttMove  = ttHit ? tte->move() : Move::none();
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
                r -= statScore / 8192;
            }
            r = std::clamp(r, 0, newDepth - 1);
        }

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
                    // ---- History updates on beta cutoff ----
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
