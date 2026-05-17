// Hypersion — history tables (header-only).
//
// Butterfly history (color, from→to) records "this quiet move was good /
// bad here" so the move ordering can prioritise empirically-winning moves.
// Capture history (piece, to-square, victim) does the same for captures.
//
// Updates use the standard "gravity" formula:
//     entry += bonus - entry * |bonus| / max
// which keeps values bounded in [-max, +max] without saturation.

#ifndef HYPERSION_HISTORY_H
#define HYPERSION_HISTORY_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "types.h"

namespace hypersion {

// Forward decls into Search::tunables — lets history_bonus() and
// update_history() be SPSA-tunable at runtime without a header-coupling
// explosion. Definitions live in src/search.cpp::tunables. A2-v2
// shipped BONUS_D2=16, BONUS_D1=30, CAP=2059 (+27.9 ELO @ 400g);
// A6 tested HIST_MAX and HIST_BONUS_CONST and found defaults at local
// optimum (SPSA converged with zero movement). Re-tunable at runtime
// via `setoption name Tune_<NAME> value <int>`.
namespace Search::tunables {
extern int HIST_BONUS_DEPTH2;
extern int HIST_BONUS_DEPTH1;
extern int HIST_BONUS_CAP;
extern int HIST_MAX;
extern int HIST_BONUS_CONST;
// Malus split (#116 follow-up, 2026-05-17). Defaults mirror bonus
// magnitudes so behavior is bit-identical until SPSA moves them.
extern int HIST_MALUS_DEPTH2, HIST_MALUS_DEPTH1, HIST_MALUS_CAP, HIST_MALUS_CONST;
}

inline int history_bonus(int depth) {
    return std::min(Search::tunables::HIST_BONUS_CAP,
                    Search::tunables::HIST_BONUS_DEPTH2 * depth * depth
                    + Search::tunables::HIST_BONUS_DEPTH1 * depth
                    + Search::tunables::HIST_BONUS_CONST);
}

// 2026-05-17 audit #116 follow-up: separate malus formula. SF18 uses
// ~1.6x bonus_cap; Hypersion ships with defaults = bonus values so the
// existing tuning is preserved at launch. SPSA campaign can move
// HIST_MALUS_* outward to find the right ratio for Hypersion's surrounding
// margins.
inline int history_malus(int depth) {
    return std::min(Search::tunables::HIST_MALUS_CAP,
                    Search::tunables::HIST_MALUS_DEPTH2 * depth * depth
                    + Search::tunables::HIST_MALUS_DEPTH1 * depth
                    + Search::tunables::HIST_MALUS_CONST);
}

inline void update_history(int& entry, int bonus) {
    const int hmax = Search::tunables::HIST_MAX;
    bonus = std::clamp(bonus, -hmax, hmax);
    entry += bonus - entry * std::abs(bonus) / hmax;
}

// `decay_buffer`: halve every int in a flat history table. Cheap and keeps
// cross-game history signal warm at the start of a new game (zeroing throws
// away strong learned patterns; halving fades them gently).
template<typename T, size_t N>
inline void decay_buffer(T (&buf)[N]) {
    int* p = reinterpret_cast<int*>(&buf[0]);
    size_t n = sizeof(buf) / sizeof(int);
    for (size_t i = 0; i < n; ++i) p[i] /= 2;
}

// Butterfly: indexed by [color][from][to].
struct ButterflyHistory {
    int data[COLOR_NB][SQUARE_NB][SQUARE_NB] = {};
    void clear() { std::memset(data, 0, sizeof(data)); }
    void decay() { decay_buffer(data); }
    int  get(Color c, Move m) const { return data[c][m.from_sq()][m.to_sq()]; }
    void update(Color c, Move m, int bonus) { update_history(data[c][m.from_sq()][m.to_sq()], bonus); }
};

// Capture history: [piece moved][to-square][captured piece type].
struct CaptureHistory {
    int data[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB] = {};
    void clear() { std::memset(data, 0, sizeof(data)); }
    void decay() { decay_buffer(data); }
    int  get(Piece pc, Square to, PieceType victim) const { return data[pc][to][victim]; }
    void update(Piece pc, Square to, PieceType victim, int bonus) { update_history(data[pc][to][victim], bonus); }
};

// Killer moves: 2 per ply, refilled when a quiet causes a beta cutoff.
struct KillerTable {
    Move killers[MAX_PLY + 4][2] = {};
    void clear() { for (auto& k : killers) k[0] = k[1] = Move::none(); }
    void clear_ply(int ply) { killers[ply][0] = killers[ply][1] = Move::none(); }
    void update(int ply, Move m) {
        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
    }
};

// Continuation history: [prev_piece][prev_to][curr_piece][curr_to].
// Captures the relationship between the previous move and the current move
// — counter-move history (1 ply back) and follow-up history (2 plies back).
struct ContinuationHistory {
    int data[PIECE_NB][SQUARE_NB][PIECE_NB][SQUARE_NB] = {};
    void clear() { std::memset(data, 0, sizeof(data)); }
    void decay() { decay_buffer(data); }
    int  get(Piece prevPc, Square prevTo, Piece curPc, Square curTo) const {
        return data[prevPc][prevTo][curPc][curTo];
    }
    void update(Piece prevPc, Square prevTo, Piece curPc, Square curTo, int bonus) {
        update_history(data[prevPc][prevTo][curPc][curTo], bonus);
    }
};

// Phase-2/3/6 attempts tombstone (this session, after PawnHistory fail):
//   Low-ply history (Worker member + LMR statScore read at ply<4):
//     -70.4 +/- 105.9 ELO @ 30g (clear regression).
//   Mate-threat extension (NMP-detects-mate, then ext=1 in move loop):
//     -11.6 +/- 101.6 ELO @ 30g (within noise but trending negative).
//   SE double-extension (when v < singularBeta - 150, extension=2):
//     -107.5 +/- 97.3 ELO @ 30g (search explosion — too aggressive).
//   CorrectionValue LMR adjust (r -= |corrVal| / 8192):
//     -147.2 +/- 110.5 ELO @ 30g (catastrophic — corr stored as cp*256
//     so the divisor was wildly miscalibrated).
//   Eval cache (thread_local 16K hash->Value):
//     +11.6 ELO @ 30g but -35 +/- 50 ELO @ 70/200g (i-cache pressure
//     under cutechess concurrency=6 — same pattern as PGO/-funroll-loops).
// Pattern: Hypersion at v2.1 is at a tight local optimum. Isolated
// SF18-style ports keep regressing because the engine's bonus formulas,
// eval magnitudes, and pruning constants are jointly tuned for the
// existing feature set. Forward progress requires (a) joint SPSA across
// multiple constants, (b) a stronger NNUE network, or (c) coordinated
// multi-feature ports (e.g., pawn-history + butterfly weight + bonus
// formula together) — not single-feature additions.

// Counter-move table: [prev_piece][prev_to] -> recommended reply.
struct CounterMoveTable {
    Move data[PIECE_NB][SQUARE_NB] = {};
    void clear() { std::memset(data, 0, sizeof(data)); }
    Move get(Piece prevPc, Square prevTo) const { return data[prevPc][prevTo]; }
    void set(Piece prevPc, Square prevTo, Move m) { data[prevPc][prevTo] = m; }
};

// NOTE: SF18 PawnHistory was attempted (port of src/history.h:152 +
// src/movepick.cpp:162 + src/search.cpp:1904). Result tombstone:
//   2x weight (matches SF butterfly 2x):  -46.6 +/- 105.1 ELO @ 30g
//   1x weight (matches Hypersion 1x bf):  -22.2 +/- 70.3 ELO @ 47g,
//                                         -49.0 +/- 51.4 ELO @ 100g
//   0.5x bonus multipliers (450/250):     -23.2 +/- 104.6 ELO @ 30g
//   0.25x bonus multipliers (225/125):    -94.9 +/- 100.5 ELO @ 30g
// Conclusion: pawn-structure history does not transfer to Hypersion's
// already-tuned move ordering. Hypersion's bonus formula gives larger
// updates than SF's and the existing butterfly + 1-ply contHist + 2-ply
// contHist already provides comparable signal. Adding pawn-history
// disrupts the local optimum at any tested magnitude. A future attempt
// would need joint SPSA over (bonus magnitudes, butterfly weight, contHist
// weights) — single-parameter sweeps cannot reach a new optimum.
//
// NOTE: 2026-05-07/08 SF18 "tables-cluster" experiment, attempted as a
// coordinated multi-feature port (per the joint-SPSA hint above):
//   (a) LowPlyHistory: per-ply [ply<5][from-to] table (640 KB), used in
//       MovePicker quiet scoring at low plies. Source: SF18
//       src/history.h::LowPlyHistory + src/movepick.cpp:158.
//   (b) 6-deep ContinuationHistory with SF SPSA-tuned non-monotonic
//       weights [1133, 683, 312, 582, 149, 474]/1024 (paired with LMR
//       statScore divisor /11248, matching SF18 src/search.cpp:1216-1224).
// Tested in 4 SPRT runs:
//   un-tuned tables only:        +8.7 +/- 35 ELO @ 200g  (marginal,
//                                            below +10 ship threshold)
//   slow-SPSA-tuned (9h, TC):   -75.9 +/- ~30 ELO @ 93g  (REJECT)
//   fast-SPSA-tuned (2h40m, nodes=50000): -8.7 +/- ~36 ELO @ 200g (REJECT)
// Conclusion: SPSA over 12 search constants with these tables added
// cannot reach an optimum that beats Hypersion's hand-tuned defaults.
// The tables themselves give marginal raw signal (+8.7 ELO) but the
// memory cost (~25 MB extra per thread) and ordering-pipeline complexity
// don't justify ship without a stronger gain. Reverted in commit f201186.
// Future attempts: try EITHER tables alone with SPSA on history-bonus
// formulas only (NOT search margins), OR a stronger NNUE that resets the
// eval-margin coupling.

// Correction history: tweaks the static eval based on past (best - eval) errors.
// Indexed by side-to-move and a 14-bit pawn-key fragment so positions with the
// same pawn structure share a slot. Capped to keep noise bounded.
struct CorrectionHistory {
    static constexpr int SIZE = 1 << 14;          // 16384 buckets
    static constexpr int CORR_MAX = 256;          // soft cap on stored adjustment (cp * 256)
    int data[COLOR_NB][SIZE] = {};
    void clear() { std::memset(data, 0, sizeof(data)); }
    int idx(std::uint64_t pawnKey) const { return int(pawnKey & (SIZE - 1)); }
    int  get(Color c, std::uint64_t pawnKey) const {
        return data[c][idx(pawnKey)];
    }
    void update(Color c, std::uint64_t pawnKey, int diff, int weight) {
        // Exponential moving average toward `diff`.
        int& slot = data[c][idx(pawnKey)];
        slot = (slot * (256 - weight) + diff * weight) / 256;
        if (slot >  CORR_MAX * 256) slot =  CORR_MAX * 256;
        if (slot < -CORR_MAX * 256) slot = -CORR_MAX * 256;
    }
    // Apply correction to a raw static eval.
    Value adjust(Color c, std::uint64_t pawnKey, Value rawEval) const {
        if (rawEval == VALUE_NONE) return rawEval;
        int corr = data[c][idx(pawnKey)] / 256;
        return Value(rawEval + corr);
    }
    // Prefetch the corr-history slot. Called right after do_move so the entry
    // is hot in cache by the time the next static-eval reads it. Per
    // Stockfish do_move (src/position.cpp).
    void prefetch(Color c, std::uint64_t pawnKey) const {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(&data[c][idx(pawnKey)]);
#endif
    }

    // ── Persistent correction history (Lc0-inspired "learn from mistakes")
    // Save / load the table to/from a binary file. The file format is:
    //   4 bytes  : magic 'HCH1' (Hypersion Corr-Hist v1)
    //   4 bytes  : table size in bytes (must match sizeof(data) on load)
    //   data     : raw table bytes
    // Failures (file missing on load, write error) are non-fatal and silent
    // from the engine's perspective — the in-memory table just stays at its
    // previous state.
    static constexpr std::uint32_t MAGIC = 0x31484348u;  // 'H','C','H','1' little-endian

    bool save_to_file(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::uint32_t magic = MAGIC;
        std::uint32_t size  = std::uint32_t(sizeof(data));
        bool ok = std::fwrite(&magic, 4, 1, f) == 1
               && std::fwrite(&size,  4, 1, f) == 1
               && std::fwrite(data,   1, sizeof(data), f) == sizeof(data);
        std::fclose(f);
        return ok;
    }
    bool load_from_file(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::uint32_t magic = 0, size = 0;
        bool header_ok = std::fread(&magic, 4, 1, f) == 1
                      && std::fread(&size,  4, 1, f) == 1
                      && magic == MAGIC
                      && size  == std::uint32_t(sizeof(data));
        if (!header_ok) { std::fclose(f); return false; }
        bool data_ok = std::fread(data, 1, sizeof(data), f) == sizeof(data);
        std::fclose(f);
        return data_ok;
    }
    // Halve all entries — applied once at load to fade old data slightly.
    // Without this, history accumulated against weak opponents could bias
    // play against stronger ones. Per-game decay (decay_for_new_game) still
    // halves on every `ucinewgame`; this just adds one extra halving on
    // engine startup as a safety net for very long-lived corrhist files.
    void halve() {
        for (int c = 0; c < COLOR_NB; ++c)
            for (int i = 0; i < SIZE; ++i)
                data[c][i] /= 2;
    }
};

}  // namespace hypersion

#endif  // HYPERSION_HISTORY_H
