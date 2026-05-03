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
    bool    operator<(const RootMove& other) const { return score > other.score; }
};

// Per-ply state passed down the search recursion.
struct Stack {
    Move  currentMove       = Move::none();
    Move  excludedMove      = Move::none();
    Piece movedPiece        = NO_PIECE;
    Value staticEval        = VALUE_NONE;
    int   moveCount         = 0;
    int   ply               = 0;
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

    // History & killers — owned per-thread.
    ButterflyHistory mainHist;
    CaptureHistory   captureHist;
    KillerTable      killers;
    CounterMoveTable counterMoves;
    CorrectionHistory pawnCorrHist;
    // Continuation-history tables, one per lookback distance.
    // contHist[i] tracks (prev-(i+1)-ply move) -> current-move bonus.
    // contHist[0] = 1-ply lookback (counter-move history).
    // contHist[1] = 2-ply lookback (follow-up history) — updated on cutoff
    // but currently NOT read for quiet ordering: a Phase-4 experiment with
    // 4-ply lookback + decaying weights (1, 1/2, 1/4, 1/8) regressed -26 ELO.
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

}  // namespace hypersion::Search

#endif  // HYPERSION_SEARCH_H
