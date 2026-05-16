// Hypersion — search core (PVS + aspiration windows + TT + MovePicker + Lazy SMP).

#ifndef HYPERSION_SEARCH_H
#define HYPERSION_SEARCH_H

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "history.h"
#include "position.h"
#include "timeman.h"
#include "types.h"

namespace hypersion::Search {

// PV display buffer — long PVs aren't useful and cost stack. Trim well below MAX_PLY.
constexpr int PV_BUFFER = 64;
struct PVLine {
    int  length = 0;
    Move moves[PV_BUFFER] = {};
};

struct RootMove {
    Move    pv0      = Move::none();
    Value   score    = -VALUE_INFINITE;
    Value   prevScore= -VALUE_INFINITE;
    int     selDepth = 0;
    PVLine  pv;
    // SF18-style "effort" tracking: number of nodes consumed while this root
    // move was the one being explored. Used by `bestMoveEffort` time scaling
    // — when one root move dominates the search, time can be saved.
    std::uint64_t effort = 0;
    // SF18-style Syzygy rank from tb_probe_root_dtz. 1000 = TB-winning AND
    // DTZ-optimal; lower = winning-but-slower; <=0 = draw/loss. Default 0
    // means "not set" so non-TB positions sort by score alone. Set at root
    // by the Syzygy block in iterative_deepen() before search begins.
    int     tbRank   = 0;
    // Sort order:
    //   1. Descending tbRank (TB-best moves first)
    //   2. Descending score (NNUE eval tiebreak within TB-equal moves)
    // This puts the TB-optimal move at position 0 of rootMoves regardless
    // of what NNUE thinks, and uses NNUE to order the rest. Crucial for
    // endgame conversion at low TC where NNUE alone gets the WDL right but
    // picks a slow DTZ move.
    bool    operator<(const RootMove& other) const {
        if (tbRank != other.tbRank) return tbRank > other.tbRank;
        return score > other.score;
    }
};

// Per-ply state passed down the search recursion.
struct Stack {
    Move  currentMove       = Move::none();
    Move  excludedMove      = Move::none();
    Piece movedPiece        = NO_PIECE;
    Value staticEval        = VALUE_NONE;
    int   moveCount         = 0;
    int   ply               = 0;
    int   cutoffCnt         = 0;   // SF18: count of fail-highs in this
                                   // node's child subtrees. Read by parent
                                   // in LMR — concentrated cutoffs in
                                   // siblings hint we should reduce more.
                                   // Source: SF18 src/search.cpp:699,1208,1374.
    int   reduction         = 0;   // SF18: how much LMR reduced the search
                                   // we just spawned at (ss+1). Child reads
                                   // (ss-1)->reduction at entry to decide
                                   // whether to bump its own depth back up
                                   // (we may have under-searched a stable
                                   // line) or down (we over-searched a
                                   // not-so-interesting one).
                                   // Source: SF18 src/search.cpp:696-697.
    int   statScore         = 0;   // SF18: cached LMR statScore for the
                                   // current move (history sum: 2*mainHist +
                                   // contHist0 + contHist1, or capture
                                   // history). Stored on stack so the
                                   // child's history-update can fold in
                                   // (ss-1)->statScore / 32 as parent-quality
                                   // feedback. Source: SF18 src/search.cpp:
                                   // 698, 1216-1224, 1834.
    bool  ttPv              = false;
    bool  inCheck           = false;
};

// Forward declare for the back-pointer.
class ThreadPool;

class Worker {
public:
    Worker();
    ~Worker();
    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;

    // Set up but don't launch. Each worker copies the source position, then
    // join() / launch() control its lifecycle.
    void prepare(const Position& pos, const SearchLimits& limits, ThreadPool* pool,
                 int threadId, bool isMain);
    void launch();
    void stop();
    void wait_for_finish();
    bool running() const { return isRunning.load(std::memory_order_acquire); }

    void clear();
    void decay_for_new_game();                     // halve persistent histories (ucinewgame)
    void reset_clock() { tm.reset_start(); }       // restart clock (ponderhit)
    std::uint64_t nodes_searched() const { return nodes.load(); }

    // Persistent correction history I/O (Lc0-inspired online learning).
    // Save / load both corr-history tables to a single file. File format:
    // 4-byte magic 'HCP1' + pawnCorrHist blob + materialCorrHist blob.
    bool save_corr_hist(const std::string& path) const;
    bool load_corr_hist(const std::string& path);

    // Best root-move data exposed for ThreadPool::best_move().
    int           completed_depth() const { return completedDepth; }
    Value         root_score()     const { return rootMoves.empty() ? VALUE_NONE : rootMoves[0].score; }
    Move          root_move()      const { return rootMoves.empty() ? Move::none() : rootMoves[0].pv0; }
    const RootMove* best_root()    const { return rootMoves.empty() ? nullptr : &rootMoves[0]; }

private:
    Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth,
                 PVLine& pv, bool isPv, bool cutNode);
    Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, bool isPv);
    void  iterative_deepen(Position& pos);
    bool  should_stop();
    void  print_info(int depth, int selDepth, Value score, std::uint64_t totalNodes,
                     TimePoint elapsed, const PVLine& pv,
                     int pvIdx = 0, int multiPv = 1, int boundFlag = 0);
                     // boundFlag: 0 = exact, 1 = upperbound (fail-low), 2 = lowerbound (fail-high)

    std::thread          th;
    std::atomic<bool>    isRunning{false};
    std::atomic<bool>    stopFlag {false};

    ThreadPool*          pool      = nullptr;
    int                  threadId  = 0;
    bool                 isMain    = true;

    Position             rootPos;
    StateInfo            rootStates[MAX_GAME_PLIES];
    int                  rootStatePly  = 0;
    SearchLimits         limits;
    TimeManager          tm;
    std::atomic<std::uint64_t> nodes{0};

    std::vector<RootMove> rootMoves;
    int                   selDepth       = 0;
    int                   completedDepth = 0;
    // NOTE: tried SF18-style nmpMinPly + verification search at depth>=16.
    // Result: -20.9 +/- 38.3 ELO at 200g 5+0.05.  Hypersion's existing
    // NMP material guard already catches the dangerous false-cutoff cases;
    // verification just adds work without compensating accuracy gain.
    // Reverted, but a future contributor exploring NMP refinement should
    // pair the verification with a paired R bump (Round-5 attempted R=5
    // alone and got -50; verification might compensate IF Hypersion's
    // surrounding params are also re-tuned).

    // History & killers — owned per-thread.
    ButterflyHistory mainHist;
    CaptureHistory   captureHist;
    KillerTable      killers;
    CounterMoveTable counterMoves;
    CorrectionHistory pawnCorrHist;
    CorrectionHistory materialCorrHist;   // SF18-style: a second correction
                                          // source keyed by material distribution
                                          // rather than pawn structure.
    // NOTE: 2026-05-12 added minorCorrHist + nonPawnCorrHist[2] with SF18
    // weight blend. LTC 20g cumulative -34.9 ± 111 ELO when bundled with
    // other SF18 ports. Reverted. Tables stay declared as dead code in case
    // a future contributor wants to retry with single-port discipline.
    // Continuation-history tables, one per lookback distance.
    // contHist[i] tracks (prev-(i+1)-ply move) -> current-move bonus.
    // contHist[0] = 1-ply lookback (counter-move history).
    // contHist[1] = 2-ply lookback (follow-up history) — updated on cutoff
    // but currently NOT read for quiet ordering: a Phase-4 experiment with
    // 4-ply lookback + decaying weights (1, 1/2, 1/4, 1/8) regressed -26 ELO.
    //
    // NOTE: re-tested 6-deep with SF18's SPSA-tuned non-monotonic weights
    // [1133, 683, 312, 582, 149, 474]/1024 (matching SF18 src/search.cpp:
    // 1877-1888 conthist_bonuses array). LMR statScore read 5 of 6 levels
    // (skip index 4) with /11248 divisor matching SF tuning. Result:
    //   30g:  -34.9 +/- 96.0 ELO  (noise, trending negative)
    //   100g: +6.9  +/- 49.4 ELO  (lucky bounce)
    //   200g: -24.4 +/- 37.7 ELO  (REJECT, tombstone)
    // Same pattern as the earlier monotonic-weight attempt — SF's exact
    // weights don't transfer either. Likely cause: 6 tables = ~24MB per
    // thread (3x cache footprint vs 2 tables), and 6x update writes per
    // cutoff. Hypersion's current-codebase calibration around the simpler
    // 2-deep version is more important than the marginal contHist accuracy
    // gain.
    //
    // Each ContinuationHistory is ~4MB; 2 tables = ~8MB per thread.
    std::unique_ptr<ContinuationHistory> contHist[2];
};

// Pool of search worker threads. workers[0] is the main worker — the one that
// prints UCI info lines and whose result is reported back to the GUI.
class ThreadPool {
public:
    void set_size(int n);              // Resizes the helper pool. Stops any active search.
    void start(const Position& pos, const SearchLimits& limits);
    void stop_all();
    void wait_all();
    void clear_all();
    void decay_all();
    int  size() const { return int(workers.size()); }
    Worker& main_worker() { return *workers[0]; }

    // Save / load corr-history. Save uses the main worker's table. Load
    // populates all workers with the same data so all threads share the
    // learned-from-mistakes baseline. Returns true on success, false on
    // any error (and the affected tables are left in a defined state:
    // existing on save-failure, cleared on load-failure).
    bool save_corr_hist(const std::string& path) const;
    bool load_corr_hist(const std::string& path);
    std::atomic<bool>& global_stop() { return stopAll; }
    std::atomic<bool>& ponder_flag() { return pondering; }
    void  reset_clock_start();              // re-anchor each worker's TimeManager
    std::uint64_t total_nodes() const;

private:
    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<bool> stopAll{false};
public:
    std::atomic<bool> pondering{false};      // true while waiting for ponderhit
};

void init();
void shutdown();

// Convenience aliases preserved so existing call sites keep working.
extern ThreadPool Threads;
inline Worker& MainWorker() { return Threads.main_worker(); }

// SPSA-style runtime tunable setter. Returns true on a recognized name
// (and updates the internal value), false if name is unknown. UCI
// options of the form `setoption name Tune_<NAME> value <int>` route here
// from cmd_setopt. See src/search.cpp::tunables::*.
bool set_tunable(const std::string& name, int value);

}  // namespace hypersion::Search

#endif  // HYPERSION_SEARCH_H
