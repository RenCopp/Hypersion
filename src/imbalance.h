// Hypersion — Imbalance evaluation (R37).
//
// Stockfish-style polynomial material imbalance: each side scores a bonus
// based on combinations of own pieces × own pieces and own × opponent.
// The 6×6 QuadraticOurs and QuadraticTheirs matrices encode that
// 2-degree polynomial.
//
// Bishop pair is treated as a binary "have-both?" flag at index 0.
//
// Source: SF sf_10 src/material.cpp::imbalance().
//
// Hyp internal scale ≈ SF internal scale (1 pawn ≈ 100), so SF coefficients
// can be used as-is. The final bonus is divided by 16 (per SF).

#pragma once

#include "evaluate.h"
#include "position.h"
#include "types.h"

namespace hypersion::Eval {

// Material-vector indexing:
//   0: bishop pair (boolean — popcount(BISHOP) >= 2)
//   1: pawn count
//   2: knight count
//   3: bishop count
//   4: rook count
//   5: queen count
constexpr int IMB_TYPES = 6;

// Lower-triangular polynomial weights (only [pt1][pt2 <= pt1] are read).
// Hyp tunes only the most-impactful coefficients (the diagonal "own-own"
// terms and a few key off-diagonals); the rest stay as SF constants.

// SF QuadraticOurs (full 6×6, only lower triangle non-zero), divided by 16
// at apply time. Values copied from SF10 material.cpp.
constexpr int QuadraticOurs[IMB_TYPES][IMB_TYPES] = {
    // OUR pieces (indexed by lower-triangle)
    //          BP    Pawn  Knight Bishop  Rook  Queen
    /*BP   */ {1438,    0,     0,    0,    0,    0},
    /*Pawn */ {  40,   38,     0,    0,    0,    0},
    /*N    */ {  32,  255,   -62,    0,    0,    0},
    /*B    */ {   0,  104,     4,    0,    0,    0},
    /*R    */ { -26,   -2,    47,  105, -208,    0},
    /*Q    */ {-189,   24,   117,  133, -134,   -6},
};

// SF QuadraticTheirs (full 6×6, only lower triangle non-zero), divided by 16.
constexpr int QuadraticTheirs[IMB_TYPES][IMB_TYPES] = {
    //          BP    Pawn  Knight Bishop  Rook  Queen
    /*BP   */ {   0,    0,     0,    0,    0,    0},
    /*Pawn */ {  36,    0,     0,    0,    0,    0},
    /*N    */ {   9,   63,     0,    0,    0,    0},
    /*B    */ {  59,   65,    42,    0,    0,    0},
    /*R    */ {  46,   39,    24,  -24,    0,    0},
    /*Q    */ {  97,  100,   -42,  137,  268,    0},
};

inline int imbalance_score(const Position& pos) {
    int  count[2][IMB_TYPES];
    for (int c = 0; c < 2; ++c) {
        Color col = Color(c);
        count[c][0] = popcount(pos.pieces(col, BISHOP)) >= 2 ? 1 : 0;
        count[c][1] = popcount(pos.pieces(col, PAWN));
        count[c][2] = popcount(pos.pieces(col, KNIGHT));
        count[c][3] = popcount(pos.pieces(col, BISHOP));
        count[c][4] = popcount(pos.pieces(col, ROOK));
        count[c][5] = popcount(pos.pieces(col, QUEEN));
    }
    int bonus[2] = { 0, 0 };
    for (int c = 0; c < 2; ++c) {
        int o = c, t = 1 - c;
        // Skip bishop-pair index in outer loop (no degree-1 contribution).
        for (int pt1 = 1; pt1 < IMB_TYPES; ++pt1) {
            if (!count[o][pt1]) continue;
            int v = 0;
            for (int pt2 = 0; pt2 <= pt1; ++pt2) {
                v += QuadraticOurs[pt1][pt2]   * count[o][pt2]
                   + QuadraticTheirs[pt1][pt2] * count[t][pt2];
            }
            bonus[c] += count[o][pt1] * v;
        }
    }
    // Scale: SF divides by 16. Apply a tunable additional scaler.
    int rawDiff = (bonus[WHITE] - bonus[BLACK]) / 16;
    return rawDiff;
}

}   // namespace hypersion::Eval
