// Hypersion — runtime-mutable evaluation parameters.
//
// The tunable SCALAR constants from evaluate.cpp live in this struct so the
// Texel tuner can mutate them at runtime. Arrays (PSQT, mobility tables,
// king-safety curve) stay constexpr in evaluate.cpp for now — they're a
// bigger refactor and the scalars are where the easy ELO is.
//
// Defaults match the values that were `constexpr int` in evaluate.cpp before
// the refactor.

#pragma once

namespace hypersion::Eval {

struct Params {
    // Tuned values from Texel run on 1M positions (8 sweeps, MSE 0.196686 → 0.196323).
    // Pawn structure
    int IsolatedPawnPenalty   = 45;
    int DoubledPawnPenalty    = 4;
    int BackwardPawnPenalty   = 39;

    // Bishop pair (tapered)
    int BishopPairBonusMG     = -2;
    int BishopPairBonusEG     = 82;

    // Rook on (semi-)open file (tapered)
    int RookOpenFileMG        = 57;
    int RookOpenFileEG        = 44;
    int RookSemiOpenFileMG    = 6;
    int RookSemiOpenFileEG    = 0;

    // Knight / bishop outposts (tapered)
    int KnightOutpostMG       = 41;
    int KnightOutpostEG       = 54;
    int BishopOutpostMG       = 42;
    int BishopOutpostEG       = 44;

    // Threats / hanging
    int HangingPenaltyMG      = 82;
};

// Single global instance; mutable. Default-constructed with the values
// above; the tuner overwrites fields and re-evaluates.
inline Params& params() {
    static Params p;
    return p;
}

}  // namespace hypersion::Eval
