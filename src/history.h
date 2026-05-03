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
#include <cstring>

#include "types.h"

namespace hypersion {

constexpr int HISTORY_MAX = 7183;   // Stockfish-tuned cap

// Lynx-style separate bonus / malus formulas (EvaluationConstants.cs:66/72 in
// the Lynx repo, with constants from Configuration.cs that have survived
// fishtest tuning). Hypersion previously used `history_bonus(d)` for both
// the cutoff-move bonus and the failed-sibling penalty — but Lynx tunes them
// independently and the malus formula has a steeper quadratic, lower cap:
//   bonus(d) = min(2440, 243 + 178·d + 3·d²)
//   malus(d) = min(1473, 220 + 253·d + 7·d²)
inline int history_bonus(int depth) {
    return std::min(2440, 243 + 178 * depth + 3 * depth * depth);
}
inline int history_malus(int depth) {
    return std::min(1473, 220 + 253 * depth + 7 * depth * depth);
}

inline void update_history(int& entry, int bonus) {
    bonus = std::clamp(bonus, -HISTORY_MAX, HISTORY_MAX);
    entry += bonus - entry * std::abs(bonus) / HISTORY_MAX;
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

// `gravity_buffer`: multiply every int by 3/4. Lynx (Helpers.cs:405) applies
// this at the start of every search call to the quiet histories only — fades
// stale per-position bias without throwing away learned patterns.
template<typename T, size_t N>
inline void gravity_buffer(T (&buf)[N]) {
    int* p = reinterpret_cast<int*>(&buf[0]);
    size_t n = sizeof(buf) / sizeof(int);
    for (size_t i = 0; i < n; ++i) p[i] = p[i] * 3 / 4;
}

// Butterfly: indexed by [color][from][to][fromAttacked][toAttacked].
// The two trailing dimensions are Lynx-style threats indexing — whether the
// move's source/target squares are attacked by the opponent. Same source +
// destination but in different threat contexts becomes a distinct slot, so
// "knight escapes attack" and "knight wanders into attack" don't pollute
// each other's history score. ~128 KB per table, vs 32 KB without threats.
struct ButterflyHistory {
    int data[COLOR_NB][SQUARE_NB][SQUARE_NB][2][2] = {};
    void clear() { std::memset(data, 0, sizeof(data)); }
    void decay() { decay_buffer(data); }
    void gravity() { gravity_buffer(data); }
    int  get(Color c, Move m, int fromAtt, int toAtt) const {
        return data[c][m.from_sq()][m.to_sq()][fromAtt][toAtt];
    }
    void update(Color c, Move m, int fromAtt, int toAtt, int bonus) {
        update_history(data[c][m.from_sq()][m.to_sq()][fromAtt][toAtt], bonus);
    }
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

// Counter-move table: [prev_piece][prev_to] -> recommended reply.
struct CounterMoveTable {
    Move data[PIECE_NB][SQUARE_NB] = {};
    void clear() { std::memset(data, 0, sizeof(data)); }
    Move get(Piece prevPc, Square prevTo) const { return data[prevPc][prevTo]; }
    void set(Piece prevPc, Square prevTo, Move m) { data[prevPc][prevTo] = m; }
};

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
};

}  // namespace hypersion

#endif  // HYPERSION_HISTORY_H
