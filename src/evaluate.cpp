// Hypersion — classical evaluator.
// Material + tapered PSQT + mobility + king safety + pawn structure + threats.

#include "evaluate.h"

#include "bitboard.h"
#include "eval_params.h"
#include "imbalance.h"
#include "kpk_bitbase.h"
#include "nnue.h"

namespace hypersion::Eval {

namespace {

// ---------- PSQTs (Stockfish-derived, mid- and end-game tapered) ----------
constexpr int PSQ_PawnMG[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
     -6,  -4,   1,   1,   1,   1,  -4,  -6,
     -9, -15,  11,  15,  15,  11, -15,  -9,
     -8,  -1,  12,  20,  20,  12,  -1,  -8,
     -2,  10,   3,  10,  10,   3,  10,  -2,
     20,  10,  20,  35,  35,  20,  10,  20,
     30,  30,  35,  45,  45,  35,  30,  30,
      0,   0,   0,   0,   0,   0,   0,   0
};
constexpr int PSQ_PawnEG[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
     -10, -6, 10,   0,  14,   7,  -5, -19,
     -10,-10,-10,   4,   4,   3, -6, -4,
       6,  -2,  -8,  -4,  -13,-12, -10, -9,
      10,   5,   4,  -5,  -5,  -5,  14,   9,
      28,  20,  21,  28,  30,   7,   6,  13,
      -1,  -6,  18,  22,  22,  17,   6,  -3,
       0,   0,   0,   0,   0,   0,   0,   0
};
constexpr int PSQ_KnightMG[64] = {
   -175,-92,-74,-73,-73,-74,-92,-175,
    -77,-41,-27,-15,-15,-27,-41, -77,
    -61,-17,  6, 12, 12,  6,-17, -61,
    -35,  8, 40, 49, 49, 40,  8, -35,
    -34, 13, 44, 51, 51, 44, 13, -34,
     -9, 22, 58, 53, 53, 58, 22,  -9,
    -67,-27,  4, 37, 37,  4,-27, -67,
   -201,-83,-56,-26,-26,-56,-83,-201
};
constexpr int PSQ_KnightEG[64] = {
   -96,-65,-49,-21,-21,-49,-65,-96,
   -67,-54,-18,  8,  8,-18,-54,-67,
   -40,-27, -8, 29, 29, -8,-27,-40,
   -35, -2, 13, 28, 28, 13, -2,-35,
   -45,-16,  9, 39, 39,  9,-16,-45,
   -51,-44,-16, 17, 17,-16,-44,-51,
   -69,-50,-51, 12, 12,-51,-50,-69,
  -100,-88,-56,-17,-17,-56,-88,-100
};
constexpr int PSQ_BishopMG[64] = {
   -53, -5, -8,-23,-23, -8, -5,-53,
   -15,  8, 19,  4,  4, 19,  8,-15,
    -7, 21, -5, 17, 17, -5, 21, -7,
    -5, 11, 25, 39, 39, 25, 11, -5,
    -12, 29, 22, 31, 31, 22, 29,-12,
    -16,  6,  1, 11, 11,  1,  6,-16,
    -17,-14,  5,  0,  0,  5,-14,-17,
    -48,  1,-14,-23,-23,-14,  1,-48
};
constexpr int PSQ_BishopEG[64] = {
   -57,-30,-37,-12,-12,-37,-30,-57,
   -37,-13,-17,  1,  1,-17,-13,-37,
   -16, -1, -2, 10, 10, -2, -1,-16,
   -20, -6,  0, 17, 17,  0, -6,-20,
   -17, -1,-14, 15, 15,-14, -1,-17,
   -30,  6,  4,  6,  6,  4,  6,-30,
   -31,-20, -1,  1,  1, -1,-20,-31,
   -46,-42,-37,-24,-24,-37,-42,-46
};
constexpr int PSQ_RookMG[64] = {
   -31,-20,-14, -5, -5,-14,-20,-31,
   -21,-13, -8,  6,  6, -8,-13,-21,
   -25,-11, -1,  3,  3, -1,-11,-25,
   -13, -5, -4, -6, -6, -4, -5,-13,
   -27,-15, -4,  3,  3, -4,-15,-27,
   -22, -2,  6, 12, 12,  6, -2,-22,
    -2, 12, 16, 18, 18, 16, 12, -2,
   -17,-19, -1,  9,  9, -1,-19,-17
};
constexpr int PSQ_RookEG[64] = {
    -9,-13,-10, -9, -9,-10,-13, -9,
   -12, -9, -1, -2, -2, -1, -9,-12,
     6, -8, -2, -6, -6, -2, -8,  6,
    -6,  1, -9,  7,  7, -9,  1, -6,
    -5,  8,  7, -6, -6,  7,  8, -5,
     6,  1, -7, 10, 10, -7,  1,  6,
     4,  5, 20, -5, -5, 20,  5,  4,
    18,  0, 19, 13, 13, 19,  0, 18
};
constexpr int PSQ_QueenMG[64] = {
     3, -5, -5,  4,  4, -5, -5,  3,
    -3,  5,  8, 12, 12,  8,  5, -3,
    -3,  6, 13,  7,  7, 13,  6, -3,
     4,  5,  9,  8,  8,  9,  5,  4,
     0, 14, 12,  5,  5, 12, 14,  0,
    -4, 10,  6,  8,  8,  6, 10, -4,
    -5,  6, 10,  8,  8, 10,  6, -5,
    -2, -2,  1, -2, -2,  1, -2, -2
};
constexpr int PSQ_QueenEG[64] = {
   -69,-57,-47,-26,-26,-47,-57,-69,
   -55,-31,-22, -4, -4,-22,-31,-55,
   -39,-18, -9,  3,  3, -9,-18,-39,
   -23, -3, 13, 24, 24, 13, -3,-23,
   -29, -6,  9, 21, 21,  9, -6,-29,
   -38,-18,-11,  1,  1,-11,-18,-38,
   -50,-27,-24, -8, -8,-24,-27,-50,
   -75,-52,-43,-36,-36,-43,-52,-75
};
constexpr int PSQ_KingMG[64] = {
   271,327,271,198,198,271,327,271,
   278,303,234,179,179,234,303,278,
   195,258,169,120,120,169,258,195,
   164,190,138, 98, 98,138,190,164,
   154,179,105, 70, 70,105,179,154,
   123,145, 81, 31, 31, 81,145,123,
    88,120, 65, 33, 33, 65,120, 88,
    59, 89, 45, -1, -1, 45, 89, 59
};
constexpr int PSQ_KingEG[64] = {
     1, 45, 85, 76, 76, 85, 45,  1,
    53,100,133,135,135,133,100, 53,
    88,130,169,175,175,169,130, 88,
   103,156,172,172,172,172,156,103,
    96,166,199,199,199,199,166, 96,
    92,172,184,191,191,184,172, 92,
    47,121,116,131,131,116,121, 47,
    11, 59, 73, 78, 78, 73, 59, 11
};

const int* const PSQ_MG[PIECE_TYPE_NB] = {
    nullptr, PSQ_PawnMG, PSQ_KnightMG, PSQ_BishopMG, PSQ_RookMG, PSQ_QueenMG, PSQ_KingMG, nullptr
};
const int* const PSQ_EG[PIECE_TYPE_NB] = {
    nullptr, PSQ_PawnEG, PSQ_KnightEG, PSQ_BishopEG, PSQ_RookEG, PSQ_QueenEG, PSQ_KingEG, nullptr
};

// Phase: 24 = full midgame, 0 = endgame.
constexpr int PhaseValues[PIECE_TYPE_NB] = { 0, 0, 1, 1, 2, 4, 0, 0 };
constexpr int MaxPhase = 24;

inline Square mirror(Square s, Color c) { return c == WHITE ? s : Square(int(s) ^ 56); }

// ---------- Mobility tables (centipawns per legal-move target square) ----------
constexpr int MobilityMG[PIECE_TYPE_NB][32] = {
    {}, {},
    /*N*/ {-62,-53,-12, -4,  3, 13, 22, 28, 33},
    /*B*/ {-48,-20, 16, 26, 38, 51, 55, 63, 63, 68, 81, 81, 91, 98},
    /*R*/ {-58,-27,-15,-10, -5, -2,  9, 16, 30, 29, 32, 38, 46, 48, 58},
    /*Q*/ {-39,-21,  3,  3, 14, 22, 28, 41, 43, 48, 56, 60, 60, 66, 67, 70,
            71, 73, 79, 88, 88, 99,102,102,106,109,113,116},
    {}, {}
};
constexpr int MobilityEG[PIECE_TYPE_NB][32] = {
    {}, {},
    /*N*/ {-81,-56,-30,-14,  8, 15, 23, 27, 33},
    /*B*/ {-59,-23, -3, 13, 24, 42, 54, 57, 65, 73, 78, 86, 88, 97},
    /*R*/ {-76,-18, 28, 55, 69, 82,112,118,132,142,155,165,166,169,171},
    /*Q*/ {-36,-15,  8, 18, 34, 54, 61, 73, 79, 92, 94,104,113,120,123,126,
           133,136,140,143,148,166,170,175,184,191,206,212},
    {}, {}
};

// ---------- King safety ----------
constexpr int KingAttackerWeight[PIECE_TYPE_NB] = { 0, 0, 81, 52, 44, 10, 0, 0 };

// Safe-check weights: per safe-check square (a square attacked by us, that
// would check them's king, that them does NOT defend). Conservative values
// that integrate with the SafetyMargin curve without overwhelming it. CRITICAL:
// these contribute to attackUnits[c] but DO NOT bump attackerCount[c] — the
// safety penalty triggers on `attackerCount >= 2` from king-zone attacks alone.
// Doubling that count was the v1 regression (-89 ELO).
constexpr int SafeCheckWeight[PIECE_TYPE_NB] = { 0, 0, 30, 15, 35, 25, 0, 0 };

constexpr int SafetyMargin[100] = {
      0,   0,   1,   2,   3,   5,   7,   9,  12,  15,
     18,  22,  26,  30,  35,  39,  44,  50,  56,  62,
     68,  75,  82,  85,  89,  97, 105, 113, 122, 131,
    140, 150, 169, 180, 191, 202, 213, 225, 237, 248,
    260, 272, 283, 295, 307, 319, 330, 342, 354, 366,
    377, 389, 401, 412, 424, 436, 448, 459, 471, 483,
    494, 500, 500, 500, 500, 500, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500
};

// ---------- Pawn structure ----------
// Tunable scalars now live in eval_params.h. Read via params() inside the
// per-position evaluate() function so the Texel tuner can mutate them.
constexpr int PassedRankBonus[8]    = { 0,  10, 17, 15, 62, 168, 276, 0 };  // by relative rank

// Connected pawn bonus by relative rank — phalanx + supported pawns.
constexpr int ConnectedPawnBonus[8] = { 0, 7, 8, 12, 29, 48, 86, 0 };

// Threats: attacking an enemy piece worth more than the attacker. Indexed by
// attacked-piece-type. Values are conservative.
constexpr int ThreatByMinorMG[PIECE_TYPE_NB] = { 0, 5, 30, 30, 50, 80, 0, 0 };
constexpr int ThreatByRookMG [PIECE_TYPE_NB] = { 0, 3, 20, 30, 0, 70, 0, 0 };
// Pawn threats: a pawn attacking a piece is among the highest-value tactical
// signals (pawns are the cheapest attacker, so the threat must be resolved).
// Indexed by attacked-piece-type. Stockfish uses values around these.
constexpr int ThreatByPawnMG[PIECE_TYPE_NB] = { 0, 0, 80, 80, 110, 130, 0, 0 };
constexpr int ThreatByPawnEG[PIECE_TYPE_NB] = { 0, 0, 50, 50,  60,  80, 0, 0 };
// HangingPenaltyMG is now in params() for tuning.

// Pawn shelter penalty — count of "missing or distant pawns" on the 3 files
// adjacent to our king, scaled by king location danger.
constexpr int PawnShelterMissingMG[4] = { 0, 12, 28, 60 };

inline Bitboard outpost_ranks(Color c) {
    return c == WHITE ? (Rank4BB | Rank5BB | Rank6BB) : (Rank3BB | Rank4BB | Rank5BB);
}

constexpr Bitboard FileBBs[8] = {
    FileABB, FileBBB, FileCBB, FileDBB, FileEBB, FileFBB, FileGBB, FileHBB
};

inline Bitboard adjacent_files_bb(File f) {
    Bitboard b = 0;
    if (f > FILE_A) b |= FileBBs[f - 1];
    if (f < FILE_H) b |= FileBBs[f + 1];
    return b;
}

inline Bitboard forward_ranks_bb(Color c, Rank r) {
    Bitboard b = 0;
    if (c == WHITE)
        for (Rank rr = Rank(r + 1); rr <= RANK_8; rr = Rank(rr + 1)) b |= rank_bb(rr);
    else
        for (Rank rr = Rank(r - 1); rr >= RANK_1; rr = Rank(rr - 1)) b |= rank_bb(rr);
    return b;
}

inline Bitboard passed_pawn_span(Color c, Square s) {
    Bitboard fwd = forward_ranks_bb(c, rank_of(s));
    return fwd & (FileBBs[file_of(s)] | adjacent_files_bb(file_of(s)));
}

inline Bitboard pawn_attacks_color(Color c, Bitboard pawns) {
    return c == WHITE ? pawn_attacks_bb<WHITE>(pawns) : pawn_attacks_bb<BLACK>(pawns);
}

// Mask of all ranks the pawn can be SUPPORTED from (same rank or strictly
// behind, in c's perspective). For a white pawn on r5: ranks 1..5. For a
// black pawn on r4: ranks 4..8. Used by backward-pawn detection — a pawn
// is potentially supported if there's a friendly pawn on an adjacent file
// at one of these ranks.
inline Bitboard same_or_behind_ranks(Color c, Rank r) {
    return ~forward_ranks_bb(c, r);   // forward = strictly in front
}

// Backward pawn (Stockfish definition):
//   - the stop square (one square ahead) is attacked by an enemy pawn, AND
//   - no friendly pawn on an adjacent file at our rank or behind it (so we
//     can't safely advance with support).
// Returns a bitboard of all backward pawns of color c.
inline Bitboard backward_pawns(Color c, Bitboard ourPawns, Bitboard theirPawnAttacks) {
    Bitboard b = ourPawns;
    Bitboard out = 0;
    while (b) {
        Square s = pop_lsb(b);
        Square stop = c == WHITE ? Square(int(s) + 8) : Square(int(s) - 8);
        if (stop < SQ_A1 || stop > SQ_H8) continue;     // shouldn't happen for legal pawns
        if (!(theirPawnAttacks & square_bb(stop))) continue;   // stop sq safe — not backward

        Bitboard supportZone = adjacent_files_bb(file_of(s)) & same_or_behind_ranks(c, rank_of(s));
        if (!(ourPawns & supportZone))
            out |= square_bb(s);
    }
    return out;
}

// Mobility area: every square except those that would be a bad-square for our
// own king or are blocked by our own pawns / king (Stockfish-style).
inline Bitboard mobility_area(const Position& pos, Color c) {
    Bitboard pawns = pos.pieces(c, PAWN);
    Bitboard king  = pos.pieces(c, KING);
    Bitboard enemyPawnAttacks = pawn_attacks_color(~c, pos.pieces(~c, PAWN));
    Bitboard blockedRanks = c == WHITE ? (Rank2BB | Rank3BB) : (Rank7BB | Rank6BB);
    Bitboard lowPawns = pawns & blockedRanks;     // back-rank pawns are immobile-ish
    return ~(lowPawns | king | enemyPawnAttacks);
}

}  // namespace

void init() {
    // KPK bitbase: ~24 KB lookup table built once at startup via retrograde
    // analysis. Probed at eval to scale KP-vs-K endings correctly (no
    // chance KP-vs-K eval thinks a known-draw is winning).
    KPK::init();
}

Value evaluate(const Position& pos) {
    // Stockfish 18 NNUE first if a network is loaded (via UCI EvalFile /
    // EvalFileSmall). Falls through to classical otherwise.
    if (NNUE::is_loaded())
        return NNUE::evaluate(pos);

    int mg = 0, eg = 0;
    int phase = 0;

    Bitboard occupied = pos.pieces();
    Bitboard pawns[2]  = { pos.pieces(WHITE, PAWN), pos.pieces(BLACK, PAWN) };
    Square   ksq[2]    = { pos.square<KING>(WHITE), pos.square<KING>(BLACK) };

    // King attack zone: 3x3 around king (king's square + king attacks).
    Bitboard kingZone[2] = {
        PseudoAttacks[KING][ksq[WHITE]] | square_bb(ksq[WHITE]),
        PseudoAttacks[KING][ksq[BLACK]] | square_bb(ksq[BLACK])
    };
    int attackUnits[2] = { 0, 0 };   // weighted attackers near opponent king
    int attackerCount[2] = { 0, 0 };

    for (Color c : { WHITE, BLACK }) {
        int sign = (c == WHITE) ? 1 : -1;
        Color them = ~c;

        // Squares not attacked by enemy pawns (used for safe mobility).
        Bitboard mobilityArea = mobility_area(pos, c);

        // ---- Safe-check precomputation ----
        // For each piece type, kingChecksBy[pt] = squares from which a `c`
        // piece of type pt attacks them's king. A SAFE check is one to a
        // square them does not defend. Each safe-check square adds
        // SafeCheckWeight[pt] to attackUnits[c] (no attacker-count bump —
        // that's reserved for king-zone attacks).
        Bitboard kingChecksBy[PIECE_TYPE_NB] = {};
        Square thKsq = ksq[them];
        kingChecksBy[KNIGHT] = PseudoAttacks[KNIGHT][thKsq];
        kingChecksBy[BISHOP] = attacks_bb<BISHOP>(thKsq, occupied);
        kingChecksBy[ROOK]   = attacks_bb<ROOK>  (thKsq, occupied);
        kingChecksBy[QUEEN]  = kingChecksBy[BISHOP] | kingChecksBy[ROOK];
        // Rough "them defends" — pawn attacks + king reach. Misses piece
        // defenders (would need a 2-pass eval to be exact); accept small
        // overcount of "safe" squares as a cost of keeping the hot path fast.
        Bitboard themDefended = pawn_attacks_color(them, pos.pieces(them, PAWN))
                              | PseudoAttacks[KING][thKsq];

        // ---- Material + PSQT ----
        for (PieceType pt = PAWN; pt <= KING; ++pt) {
            Bitboard b = pos.pieces(c, pt);
            if (pt != KING) phase += popcount(b) * PhaseValues[pt];
            // Round 10: per-piece-type PSQT scalar multiplier.
            int psqtMG = (pt == PAWN)   ? params().PawnPSQTScaleMG
                       : (pt == KNIGHT) ? params().KnightPSQTScaleMG
                       : (pt == BISHOP) ? params().BishopPSQTScaleMG
                       : (pt == ROOK)   ? params().RookPSQTScaleMG
                       : (pt == QUEEN)  ? params().QueenPSQTScaleMG
                                        : params().KingPSQTScaleMG;
            int psqtEG = (pt == PAWN)   ? params().PawnPSQTScaleEG
                       : (pt == KNIGHT) ? params().KnightPSQTScaleEG
                       : (pt == BISHOP) ? params().BishopPSQTScaleEG
                       : (pt == ROOK)   ? params().RookPSQTScaleEG
                       : (pt == QUEEN)  ? params().QueenPSQTScaleEG
                                        : params().KingPSQTScaleEG;
            // Round 13: per-piece material-value scalar (KING fixed at 100).
            int valMG = (pt == PAWN)   ? params().PawnValueScaleMG
                      : (pt == KNIGHT) ? params().KnightValueScaleMG
                      : (pt == BISHOP) ? params().BishopValueScaleMG
                      : (pt == ROOK)   ? params().RookValueScaleMG
                      : (pt == QUEEN)  ? params().QueenValueScaleMG
                                       : 100;
            int valEG = (pt == PAWN)   ? params().PawnValueScaleEG
                      : (pt == KNIGHT) ? params().KnightValueScaleEG
                      : (pt == BISHOP) ? params().BishopValueScaleEG
                      : (pt == ROOK)   ? params().RookValueScaleEG
                      : (pt == QUEEN)  ? params().QueenValueScaleEG
                                       : 100;
            while (b) {
                Square s = pop_lsb(b);
                int psqIdx = mirror(s, c);
                mg += sign * (PieceValueMG[pt] * valMG / 100 + PSQ_MG[pt][psqIdx] * psqtMG / 100);
                eg += sign * (PieceValueEG[pt] * valEG / 100 + PSQ_EG[pt][psqIdx] * psqtEG / 100);

                // ---- Knight on a/h file penalty (Round 27) ----
                // Knights on rim files have ~half their attack range.
                // Apply penalty per knight on file a or h.
                if (pt == KNIGHT) {
                    File f = file_of(s);
                    if (f == FILE_A || f == FILE_H) {
                        mg -= sign * params().KnightRimPenaltyMG;
                        eg -= sign * params().KnightRimPenaltyEG;
                    }
                }

                // ---- Rook on 8th rank (Round 30, EG only) ----
                // Beyond RookOn7th, a rook on the absolute back rank
                // (relative rank 8) is usually attacking pieces.
                if (pt == ROOK) {
                    int relRank = (c == WHITE) ? int(rank_of(s)) : 7 - int(rank_of(s));
                    if (relRank == 7) {
                        eg += sign * params().RookOn8thEG;
                    }
                }

                // ---- Mobility & king-attack tracking for sliders / knights ----
                if (pt == KNIGHT || pt == BISHOP || pt == ROOK || pt == QUEEN) {
                    Bitboard atk = (pt == KNIGHT) ? PseudoAttacks[KNIGHT][s]
                                 : (pt == BISHOP) ? attacks_bb<BISHOP>(s, occupied)
                                 : (pt == ROOK)   ? attacks_bb<ROOK>  (s, occupied)
                                                  : attacks_bb<QUEEN> (s, occupied);
                    int mobCount = popcount(atk & mobilityArea);
                    if (mobCount > 27) mobCount = 27;
                    // Round 5: per-piece-type mobility scalar (Texel-tunable).
                    // Default 100 reproduces the constexpr tables exactly.
                    int scaleMG = (pt == KNIGHT) ? params().KnightMobScaleMG
                                : (pt == BISHOP) ? params().BishopMobScaleMG
                                : (pt == ROOK)   ? params().RookMobScaleMG
                                                 : params().QueenMobScaleMG;
                    int scaleEG = (pt == KNIGHT) ? params().KnightMobScaleEG
                                : (pt == BISHOP) ? params().BishopMobScaleEG
                                : (pt == ROOK)   ? params().RookMobScaleEG
                                                 : params().QueenMobScaleEG;
                    mg += sign * MobilityMG[pt][mobCount] * scaleMG / 100;
                    eg += sign * MobilityEG[pt][mobCount] * scaleEG / 100;

                    // King-attack contribution (attacks on enemy king zone).
                    // Round 17: KingAttackerWeight is tunable per piece type.
                    Bitboard kingAttacks = atk & kingZone[them];
                    if (kingAttacks) {
                        int kaw = (pt == KNIGHT) ? params().KingAttacker_Knight
                                : (pt == BISHOP) ? params().KingAttacker_Bishop
                                : (pt == ROOK)   ? params().KingAttacker_Rook
                                                 : params().KingAttacker_Queen;
                        attackUnits[c]   += kaw * popcount(kingAttacks);
                        ++attackerCount[c];
                    }

                    // ---- Queen-king tropism (Round 31, MG only) ----
                    // Queens close to the enemy king get extra pressure
                    // weight even when not directly attacking the king
                    // zone. Distance ≤ 3 chebyshev. Off by default.
                    if (pt == QUEEN && params().QueenKingTropismMG != 0) {
                        int d = distance(s, ksq[them]);
                        if (d <= 3) {
                            mg += sign * params().QueenKingTropismMG * (4 - d);
                        }
                    }

                    // Safe-check contribution. Adds to attackUnits but does
                    // NOT increment attackerCount — that gating remains for
                    // king-zone attackers only.
                    // Round 17: SafeCheckWeight tunable per piece type.
                    Bitboard safeChecks = atk & kingChecksBy[pt] & ~themDefended;
                    if (safeChecks) {
                        int scw = (pt == KNIGHT) ? params().SafeCheck_Knight
                                : (pt == BISHOP) ? params().SafeCheck_Bishop
                                : (pt == ROOK)   ? params().SafeCheck_Rook
                                                 : params().SafeCheck_Queen;
                        attackUnits[c] += scw * popcount(safeChecks);
                    }

                    // Knight / bishop outpost: standing on a square supported by a
                    // friendly pawn and not reachable by any enemy pawn.
                    if (pt == KNIGHT || pt == BISHOP) {
                        Bitboard onOutpostRank = square_bb(s) & outpost_ranks(c);
                        Bitboard ourPawnAttacks = pawn_attacks_color(c, pawns[c]);
                        Bitboard theirPawnSpan  = passed_pawn_span(them, s);
                        if (onOutpostRank
                            && (ourPawnAttacks & square_bb(s))
                            && !(theirPawnSpan & pawns[them])) {
                            const auto& P = params();
                            mg += sign * (pt == KNIGHT ? P.KnightOutpostMG : P.BishopOutpostMG);
                            eg += sign * (pt == KNIGHT ? P.KnightOutpostEG : P.BishopOutpostEG);
                        }
                    }

                    // Reachable knight outpost (Round 3 + R7 EG split).
                    // Knight that can jump in one move to a square that is
                    // (a) an outpost rank, (b) supported by our pawn, (c)
                    // not attacked by enemy pawns. Weaker than direct
                    // outpost.
                    if (pt == KNIGHT) {
                        Bitboard ourPawnAttacks = pawn_attacks_color(c, pawns[c]);
                        Bitboard theirPawnAttacks = pawn_attacks_color(them, pawns[them]);
                        Bitboard reachable = atk
                            & outpost_ranks(c)
                            & ourPawnAttacks
                            & ~theirPawnAttacks
                            & ~pos.pieces();
                        if (reachable) {
                            mg += sign * params().ReachableOutpostMG;
                            eg += sign * params().ReachableOutpostEG;
                        }
                    }
                }
            }
        }

        // ---- Pawn islands (Round 28) ----
        // Count contiguous groups of files that contain own pawns. 1
        // island = ideal phalanx; each extra island = pawn weakness.
        {
            int islands = 0;
            bool inIsland = false;
            for (int f = 0; f < 8; ++f) {
                bool hasPawn = (pawns[c] & FileBBs[f]) != 0;
                if (hasPawn && !inIsland) ++islands;
                inIsland = hasPawn;
            }
            int extraIslands = std::max(0, islands - 1);
            mg -= sign * extraIslands * params().PawnIslandPenaltyMG;
            eg -= sign * extraIslands * params().PawnIslandPenaltyEG;
        }

        // ---- Connected pawns ----
        {
            Bitboard pw = pawns[c];
            Bitboard supported = pw & pawn_attacks_color(c, pw);   // pawn defended by another pawn
            Bitboard phalanx   = pw & (((pw & ~FileABB) >> 1) | ((pw & ~FileHBB) << 1));
            Bitboard connected = supported | phalanx;
            while (connected) {
                Square s = pop_lsb(connected);
                int relRank = c == WHITE ? int(rank_of(s)) : 7 - int(rank_of(s));
                // Round 11: indices 4/5/6 (advanced ranks 5/6/7) use
                // tunable values from eval_params.h; other indices fall
                // back to the constexpr table.
                int bonus;
                if      (relRank == 4) bonus = params().ConnectedPawnRank4;
                else if (relRank == 5) bonus = params().ConnectedPawnRank5;
                else if (relRank == 6) bonus = params().ConnectedPawnRank6;
                else                   bonus = ConnectedPawnBonus[relRank];
                mg += sign * bonus / 2;
                eg += sign * bonus;
            }
        }

        // ---- Threats: minors / rooks attacking higher-value enemy pieces ----
        {
            Bitboard minorAtkAll = 0, rookAtkAll = 0;
            Bitboard knights = pos.pieces(c, KNIGHT);
            while (knights) { Square s = pop_lsb(knights); minorAtkAll |= PseudoAttacks[KNIGHT][s]; }
            Bitboard bishops = pos.pieces(c, BISHOP);
            while (bishops) { Square s = pop_lsb(bishops); minorAtkAll |= attacks_bb<BISHOP>(s, occupied); }
            Bitboard rooks2 = pos.pieces(c, ROOK);
            while (rooks2)  { Square s = pop_lsb(rooks2); rookAtkAll  |= attacks_bb<ROOK>(s, occupied); }

            // Pawn-attacks on enemy pieces. Computed once per side.
            Bitboard pawnAtkAll = pawn_attacks_color(c, pawns[c]);
            // Round 16: high-impact threat entries are tunable via
            // eval_params.h; pawn->lower-value-piece and minor->minor
            // entries stay constexpr (low-signal).
            for (PieceType vt = KNIGHT; vt <= QUEEN; vt = PieceType(vt + 1)) {
                Bitboard victims = pos.pieces(them, vt);
                int n1 = popcount(victims & minorAtkAll);
                int n2 = popcount(victims & rookAtkAll);
                int n3 = popcount(victims & pawnAtkAll);

                // ThreatByMinor: ROOK / QUEEN tunable, others from constexpr
                int tbm = (vt == ROOK)  ? params().ThreatByMinor_Rook
                        : (vt == QUEEN) ? params().ThreatByMinor_Queen
                                        : ThreatByMinorMG[vt];
                mg += sign * n1 * tbm;

                // ThreatByRook: QUEEN tunable, others from constexpr
                int tbr = (vt == QUEEN) ? params().ThreatByRook_Queen
                                        : ThreatByRookMG[vt];
                mg += sign * n2 * tbr;

                // ThreatByPawn: N/B/R/Q tunable (MG + EG), pawn->pawn from constexpr
                int tbpMG = (vt == KNIGHT) ? params().ThreatByPawn_KnightMG
                          : (vt == BISHOP) ? params().ThreatByPawn_BishopMG
                          : (vt == ROOK)   ? params().ThreatByPawn_RookMG
                          : (vt == QUEEN)  ? params().ThreatByPawn_QueenMG
                                           : ThreatByPawnMG[vt];
                int tbpEG = (vt == KNIGHT) ? params().ThreatByPawn_KnightEG
                          : (vt == BISHOP) ? params().ThreatByPawn_BishopEG
                          : (vt == ROOK)   ? params().ThreatByPawn_RookEG
                          : (vt == QUEEN)  ? params().ThreatByPawn_QueenEG
                                           : ThreatByPawnEG[vt];
                mg += sign * n3 * tbpMG;
                eg += sign * n3 * tbpEG;
            }

            // Hanging: enemy pieces attacked by us but not defended.
            Bitboard ourAttacks = minorAtkAll | rookAtkAll
                                | pawn_attacks_color(c, pawns[c]);
            Bitboard theirDef   = pawn_attacks_color(them, pawns[them])
                                | PseudoAttacks[KING][ksq[them]];
            // (rough approximation — full attack set would require per-piece scan)
            Bitboard hanging = pos.pieces(them) & ~pos.pieces(them, PAWN) & ~pos.pieces(them, KING)
                             & ourAttacks & ~theirDef;
            int hangN = popcount(hanging);
            mg += sign * hangN * params().HangingPenaltyMG;
            // Hanging EG: even more decisive once queens come off — material
            // loss in endgame has no compensating dynamic value.
            eg += sign * hangN * params().HangingPenaltyEG;
        }

        // ---- King pawn shelter ----
        // Count missing pawns on the king's file and the two adjacent files.
        {
            int missing = 0;
            File kf = file_of(ksq[c]);
            for (int df = -1; df <= 1; ++df) {
                int f = kf + df;
                if (f < 0 || f > 7) continue;
                Bitboard fileMask = FileBBs[f];
                Bitboard ours = pawns[c] & fileMask;
                if (!ours) ++missing;
            }
            // Round 18: per-missing-count tunable pawn shelter penalty.
            if (missing == 1) mg -= sign * params().PawnShelter_1Missing;
            else if (missing == 2) mg -= sign * params().PawnShelter_2Missing;
            else if (missing >= 3) mg -= sign * params().PawnShelter_3Missing;
        }

        // ---- Pawn storm vs enemy king (Round 4, mg only) ----
        // Own pawns advanced into ranks 5-7 (white) / 4-2 (black) within
        // 2 files of the enemy king. Storming pawns lever open enemy
        // shelter. SF classical king_safety()::pawn_storm.
        {
            int kf = int(file_of(ksq[them]));
            int fLo = std::max(0, kf - 2);
            int fHi = std::min(7, kf + 2);
            Bitboard fileBand = 0;
            for (int f = fLo; f <= fHi; ++f) fileBand |= FileBBs[f];
            Bitboard advancedRanks = (c == WHITE)
                ? (Rank5BB | Rank6BB | Rank7BB)
                : (Rank4BB | Rank3BB | Rank2BB);
            int stormPawns = popcount(pawns[c] & fileBand & advancedRanks);
            mg += sign * stormPawns * params().PawnStormMG;
        }

        // ---- Knight vs Bishop imbalance (Round 4) ----
        // Closed positions (many pawns) favour knights; open positions
        // favour bishops. The bonus scales the (knight_count - bishop_count)
        // difference by total pawn count delta from 8.
        {
            int nN = popcount(pos.pieces(c, KNIGHT));
            int nB = popcount(pos.pieces(c, BISHOP));
            int totalPawns = popcount(pos.pieces(WHITE, PAWN) | pos.pieces(BLACK, PAWN));
            // closedness in [-8 (no pawns) .. +8 (16 pawns)] roughly; use
            // raw pawn count − 8 so neutral at 8 pawns.
            int closedness = totalPawns - 8;
            // Side with more knights benefits from closed; with more bishops, from open.
            int knightAdv = nN - nB;
            mg += sign * knightAdv * closedness * params().KnightVsBishopPawnsMG / 8;
            eg += sign * knightAdv * closedness * params().KnightVsBishopPawnsEG / 8;
        }

        // ---- Bishop pair bonus ----
        // Round 29: extra bonus when position is OPEN (few pawns). Each
        // pawn below 16 adds Scale% / 100 to the bonus magnitude. Disabled
        // (Scale=0) by default until tuned.
        if (popcount(pos.pieces(c, BISHOP)) >= 2) {
            int openness = 16 - popcount(pos.pieces(PAWN));   // 0..16
            int extraMG = params().BishopPairBonusMG * openness * params().BishopPairOpenScaleMG / 1600;
            int extraEG = params().BishopPairBonusEG * openness * params().BishopPairOpenScaleEG / 1600;
            mg += sign * (params().BishopPairBonusMG + extraMG);
            eg += sign * (params().BishopPairBonusEG + extraEG);
        }

        // ---- Bishop pawns on same colour (per-pawn penalty, tapered) ----
        // "Bad bishop": own pawns sitting on squares the same colour as the
        // bishop occupy its diagonals. Per SF classical evaluate.cpp.
        {
            Bitboard ourBishops = pos.pieces(c, BISHOP);
            while (ourBishops) {
                Square bs = pop_lsb(ourBishops);
                bool isDark = (square_bb(bs) & DarkSquares) != 0;
                Bitboard sameColour = isDark ? DarkSquares : ~DarkSquares;
                int bad = popcount(pawns[c] & sameColour);
                mg -= sign * bad * params().BishopPawnSCMG;
                eg -= sign * bad * params().BishopPawnSCEG;
            }
        }

        // ---- Long-diagonal bishop bonus (mg only) ----
        // Bishop on one of {d4, e4, d5, e5} that also has line of sight along
        // its long diagonal through the centre, blocked only by own pawns.
        // Approximation: it can see at least one of the other three centre
        // squares through pawn-only-as-blocker x-rays. SF classical:
        // src/evaluate.cpp::pieces<>().
        {
            constexpr Bitboard centreFour =
                  (Bitboard(1) << SQ_D4) | (Bitboard(1) << SQ_E4)
                | (Bitboard(1) << SQ_D5) | (Bitboard(1) << SQ_E5);
            Bitboard centreBishops = pos.pieces(c, BISHOP) & centreFour;
            while (centreBishops) {
                Square bs = pop_lsb(centreBishops);
                Bitboard xray = attacks_bb<BISHOP>(bs, pawns[c] | pawns[them]);
                if (xray & (centreFour ^ square_bb(bs)))
                    mg += sign * params().LongDiagBishopMG;
            }
        }

        // ---- Minor piece behind own pawn (mg only) ----
        // Knight or bishop with an own pawn one rank in front, same file.
        // Reward safe development. SF classical.
        {
            Bitboard minors = pos.pieces(c, KNIGHT) | pos.pieces(c, BISHOP);
            // Shift our pawns BACKWARD by one rank to align "pawn-in-front-of-piece"
            // with the piece's own square. For white, "behind pawn" means piece
            // at rank R, pawn at rank R+1 → pawn bit at (sq + 8) → shift pawns
            // right by 8.
            Bitboard pawnFront = (c == WHITE) ? (pawns[c] >> 8) : (pawns[c] << 8);
            int sheltered = popcount(minors & pawnFront);
            mg += sign * sheltered * params().MinorBehindPawnMG;
        }

        // ---- Trapped bishop in corner (Round 2) ----
        // Bishop on {a7,h7} (white) or {a2,h2} (black) with own pawn one
        // rank+file diagonally inward AND enemy pawn one rank further still.
        // Classic "stuck in corner" pattern from Indian-defense openings.
        // SF classical: large penalty (~-50 mg / -50 eg in SF units).
        {
            Bitboard ourBishops = pos.pieces(c, BISHOP);
            while (ourBishops) {
                Square bs = pop_lsb(ourBishops);
                int f = int(file_of(bs));
                int r = int(rank_of(bs));
                // White trapped corners: a7/h7 with own pawn on b6/g6.
                // Black trapped corners: a2/h2 with own pawn on b3/g3.
                bool isTrap = false;
                if (c == WHITE && r == 6) {
                    if (f == 0 && (pawns[c] & square_bb(SQ_B6))) isTrap = true;
                    if (f == 7 && (pawns[c] & square_bb(SQ_G6))) isTrap = true;
                } else if (c == BLACK && r == 1) {
                    if (f == 0 && (pawns[c] & square_bb(SQ_B3))) isTrap = true;
                    if (f == 7 && (pawns[c] & square_bb(SQ_G3))) isTrap = true;
                }
                if (isTrap) {
                    mg -= sign * params().TrappedBishopMG;
                    eg -= sign * params().TrappedBishopEG;
                }
            }
        }

        // ---- Pawn phalanx (Round 2 + R7 EG split) ----
        // Per pair of own pawns on the same rank, adjacent files. Distinct
        // from ConnectedPawnBonus (which keys on either phalanx OR support).
        // This is a small additional bonus for the phalanx-specific shape.
        {
            Bitboard pw = pawns[c];
            // Shift right by 1 file → pairs where bit i is in pw AND
            // bit (i+1) is in pw. Each phalanx-pair counted once.
            Bitboard phalanxPairs = pw & ((pw & ~FileABB) >> 1);
            int n = popcount(phalanxPairs);
            mg += sign * n * params().PhalanxPawnMG;
            eg += sign * n * params().PhalanxPawnEG;
        }

        // ---- Space evaluation (mg only, Round 2) ----
        // Squares in central 4 files (c..f), our half of board ranks 2-4 (W)
        // / 5-7 (B), NOT attacked by enemy pawns. SF classical scales this
        // by piece-count / blocked-pawn-count; we use the plain count as a
        // single Texel-tunable scalar to keep the addition small.
        {
            constexpr Bitboard centralFiles = FileCBB | FileDBB | FileEBB | FileFBB;
            Bitboard ourHalf = (c == WHITE) ? (Rank2BB | Rank3BB | Rank4BB)
                                            : (Rank5BB | Rank6BB | Rank7BB);
            Bitboard area = centralFiles & ourHalf;
            Bitboard theirPawnAtks = pawn_attacks_color(them, pawns[them]);
            int safeCount = popcount(area & ~theirPawnAtks);
            mg += sign * safeCount * params().SpaceAreaMG;
        }

        // ---- Rook on open / semi-open file + 7th rank (Round 3) ----
        // Rook on relative rank 7 is a strong asset (cuts off king, harasses
        // pawns). SF classical evaluate.cpp::pieces<>().
        {
            Bitboard relRank7 = (c == WHITE) ? Rank7BB : Rank2BB;
            Bitboard rooks7   = pos.pieces(c, ROOK) & relRank7;
            int n7 = popcount(rooks7);
            mg += sign * n7 * params().RookOn7thMG;
            eg += sign * n7 * params().RookOn7thEG;
        }

        // ---- Doubled rooks (Round 4) ----
        // Two rooks on the same file = strong vertical control.
        {
            Bitboard ourRooks = pos.pieces(c, ROOK);
            int doubledPairs = 0;
            for (int f = 0; f < 8; ++f) {
                int n = popcount(ourRooks & FileBBs[f]);
                if (n >= 2) ++doubledPairs;
            }
            mg += sign * doubledPairs * params().DoubledRookMG;
            eg += sign * doubledPairs * params().DoubledRookEG;
        }

        // ---- Connected rooks (Round 4 + R7 EG split) ----
        // Two rooks defending each other along a rank/file with no blocker.
        // Detection: rook A's rook-attack from its square (with occupancy)
        // intersects rook B's square.
        {
            Bitboard ourRooks = pos.pieces(c, ROOK);
            if (popcount(ourRooks) >= 2) {
                // Pick lowest rook, check if its attacks include another
                // friendly rook.
                Square r1 = pop_lsb(ourRooks);
                Bitboard r1Atk = attacks_bb<ROOK>(r1, occupied);
                if (r1Atk & ourRooks) {   // ourRooks now has other rooks only
                    mg += sign * params().ConnectedRookMG;
                    eg += sign * params().ConnectedRookEG;
                }
            }
        }

        // ---- Rook on enemy king's file (Round 6, mg only) ----
        {
            Bitboard kingFile = FileBBs[file_of(ksq[them])];
            int n = popcount(pos.pieces(c, ROOK) & kingFile);
            mg += sign * n * params().RookOnKingFileMG;
        }

        // ---- Bishop x-ray on enemy queen (Round 6, mg only) ----
        // Bishop's diagonal x-ray (no blockers) reaches the enemy queen —
        // creates pin / skewer pressure.
        {
            Bitboard ourBishops = pos.pieces(c, BISHOP);
            Bitboard enemyQueens = pos.pieces(them, QUEEN);
            int xrays = 0;
            while (ourBishops) {
                Square bs = pop_lsb(ourBishops);
                if (attacks_bb<BISHOP>(bs, 0) & enemyQueens) ++xrays;
            }
            mg += sign * xrays * params().BishopXrayQueenMG;
        }

        // ---- Open files near enemy king (Round 6, mg only) ----
        // Files within 2 of enemy king with NO own pawn = "attack runway"
        // for our heavy pieces.
        {
            int kf = int(file_of(ksq[them]));
            int fLo = std::max(0, kf - 2);
            int fHi = std::min(7, kf + 2);
            int openCount = 0;
            for (int f = fLo; f <= fHi; ++f)
                if (!(pawns[c] & FileBBs[f])) ++openCount;
            mg += sign * openCount * params().OpenFilesNearKingMG;
        }

        Bitboard rooks = pos.pieces(c, ROOK);
        while (rooks) {
            Square s = pop_lsb(rooks);
            Bitboard fileBB = FileBBs[file_of(s)];
            bool ourPawnOnFile   = pawns[c]   & fileBB;
            bool theirPawnOnFile = pawns[them] & fileBB;
            if (!ourPawnOnFile && !theirPawnOnFile) {
                mg += sign * params().RookOpenFileMG;     eg += sign * params().RookOpenFileEG;
            } else if (!ourPawnOnFile) {
                mg += sign * params().RookSemiOpenFileMG; eg += sign * params().RookSemiOpenFileEG;
            }
        }

        // ---- Passed / isolated / doubled / backward pawns ----
        // Backward set computed once per color outside the per-pawn loop —
        // it depends on the full pawn structure of both sides.
        Bitboard backward = backward_pawns(c, pawns[c],
                                           pawn_attacks_color(them, pawns[them]));
        Bitboard b = pawns[c];
        while (b) {
            Square s = pop_lsb(b);
            Bitboard sf = passed_pawn_span(c, s);
            if (!(sf & pawns[them])) {
                int relRank = (c == WHITE) ? int(rank_of(s)) : 7 - int(rank_of(s));
                // Round 14: indices 4/5/6 use eval_params; others fall
                // back to the constexpr table.
                int passedBonus;
                if      (relRank == 4) passedBonus = params().PassedRank4;
                else if (relRank == 5) passedBonus = params().PassedRank5;
                else if (relRank == 6) passedBonus = params().PassedRank6;
                else                   passedBonus = PassedRankBonus[relRank];
                eg += sign * passedBonus;
                mg += sign * passedBonus / 3;

                // Passed-pawn king-distance bonus (endgame only). Reward own
                // king for being close to the push-target square and enemy
                // king for being far from it. Skip rank-1 / rank-7 pushes
                // (pre-promotion or already-promoted edge case).
                int pushRank = (c == WHITE) ? int(rank_of(s)) + 1
                                            : int(rank_of(s)) - 1;
                if (pushRank >= 0 && pushRank <= 7) {
                    Square pushTo = Square(int(file_of(s)) + 8 * pushRank);
                    int ownD   = distance(ksq[c],    pushTo);
                    int enemyD = distance(ksq[them], pushTo);
                    eg += sign * (enemyD * params().PassedKingEnemyDistEG
                                - ownD   * params().PassedKingOwnDistEG);
                }
            }
            // Isolated: no friendly pawn on adjacent files.
            // Round 19: separate MG/EG penalty.
            if (!(adjacent_files_bb(file_of(s)) & pawns[c])) {
                mg -= sign * params().IsolatedPawnPenalty;
                eg -= sign * params().IsolatedPawnPenaltyEG;
            }
            // Doubled: another friendly pawn on same file behind.
            // Round 19: separate MG/EG penalty.
            if (popcount(pawns[c] & FileBBs[file_of(s)]) > 1) {
                mg -= sign * params().DoubledPawnPenalty / 2;
                eg -= sign * params().DoubledPawnPenaltyEG / 2;
            }
            // Backward: stop square attacked by enemy pawn, no friendly
            // pawn on adjacent file at our rank or behind to support a
            // safe advance.
            // Round 19: separate MG/EG.
            if (backward & square_bb(s)) {
                mg -= sign * params().BackwardPawnPenalty;
                eg -= sign * params().BackwardPawnPenaltyEG;
            }

            // Pawn lever (Round 3 + R7 EG split). Own pawn with an enemy
            // pawn on its diagonal capture square — creates pawn-break
            // tension.
            {
                Bitboard pawnAtkFromS = pawn_attacks_bb(c, s);
                if (pawnAtkFromS & pawns[them]) {
                    mg += sign * params().PawnLeverMG;
                    eg += sign * params().PawnLeverEG;
                }
            }

            // Candidate passed pawn (Round 3, eg only). Not yet passed
            // (some enemy pawn on file or adjacent ahead) but its
            // supporters at-or-behind outnumber the obstructors. The
            // pawn could become passed if its file opens up.
            // Skip if already passed (handled above).
            if (sf & pawns[them]) {  // sf = passed_pawn_span; has enemy → not passed
                File f = file_of(s);
                Bitboard adjFiles = adjacent_files_bb(f);
                int relRank = (c == WHITE) ? int(rank_of(s)) : 7 - int(rank_of(s));
                // Supporters: own pawns on adj files at our rank or behind.
                Bitboard ourBehindMask = (c == WHITE)
                    ? ((Bitboard(1) << ((relRank + 1) * 8)) - 1)
                    : ~((Bitboard(1) << (((7 - relRank) + 0) * 8)) - 1);
                Bitboard supporters  = pawns[c]   & adjFiles & ourBehindMask;
                // Obstructors: enemy pawns on adj files at next-rank or ahead.
                int blockerRank = (c == WHITE) ? relRank + 1 : 7 - relRank - 1;
                Bitboard theirAheadMask;
                if (c == WHITE)
                    theirAheadMask = (blockerRank >= 0 && blockerRank <= 7)
                        ? ~((Bitboard(1) << (blockerRank * 8)) - 1) : 0;
                else
                    theirAheadMask = (blockerRank >= 0 && blockerRank <= 7)
                        ? ((Bitboard(1) << ((blockerRank + 1) * 8)) - 1) : 0;
                Bitboard obstructors = pawns[them] & adjFiles & theirAheadMask;
                // No own pawn on our own file ahead (would block our advance).
                Bitboard ourFileAhead = pawns[c] & FileBBs[f]
                    & ((c == WHITE) ? ~((Bitboard(1) << ((relRank + 1) * 8)) - 1)
                                    : ((Bitboard(1) << ((7 - relRank) * 8)) - 1));
                if (!ourFileAhead
                    && popcount(supporters) >= popcount(obstructors)
                    && obstructors)   // require some obstructor — else it's already (semi-)passed
                    eg += sign * params().CandidatePawnEG;
            }
        }
    }

    // ---- Imbalance polynomial (Round 37, MG only) ----
    // Stockfish-style 6x6 quadratic material imbalance. Disabled at 0
    // scale by default until tuned.
    if (params().ImbalanceScale != 0) {
        mg += imbalance_score(pos) * params().ImbalanceScale / 100;
    }

    // ---- King safety: scale attack units through SafetyMargin curve. ----
    // Round 6: KingSafetyScale (default 100) lets Texel adjust the overall
    // weight of the king-safety contribution.
    {
        int safetyScale = params().KingSafetyScale;
        for (Color c : { WHITE, BLACK }) {
            int sign = (c == WHITE) ? 1 : -1;
            if (attackerCount[c] >= 2) {
                int idx = std::min(99, attackUnits[c] / 8);
                mg += sign * SafetyMargin[idx] * safetyScale / 100;
            }
        }
    }

    // ---- Bad bishop: own pawns on bishop's attack squares (Round 34) ----
    // Counts own pawns that sit on squares the bishop can attack (with
    // current occupancy). High count = bishop's mobility is choked by
    // own pawns — strong "bad bishop" signal beyond the same-colour
    // count.
    {
        Bitboard occBB = pos.pieces();
        for (Color c : { WHITE, BLACK }) {
            int sign = (c == WHITE) ? 1 : -1;
            Bitboard ourBishops = pos.pieces(c, BISHOP);
            Bitboard ourPawns   = pos.pieces(c, PAWN);
            int totalBlocked = 0;
            while (ourBishops) {
                Square bs = pop_lsb(ourBishops);
                Bitboard reach = attacks_bb<BISHOP>(bs, occBB);
                totalBlocked += popcount(reach & ourPawns);
            }
            mg -= sign * totalBlocked * params().BadBishopBlockedMG;
            eg -= sign * totalBlocked * params().BadBishopBlockedEG;
        }
    }

    // ---- Connected passers (Round 32) ----
    // Two passed pawns on adjacent files support each other.
    // Computed per side; bonus per adjacent-file passer pair.
    {
        for (Color c : { WHITE, BLACK }) {
            Bitboard ourPawns   = pos.pieces(c, PAWN);
            Bitboard theirPawns = pos.pieces(~c, PAWN);
            Bitboard passed = 0;
            Bitboard pw = ourPawns;
            while (pw) {
                Square s = pop_lsb(pw);
                if (!(passed_pawn_span(c, s) & theirPawns))
                    passed |= square_bb(s);
            }
            int pairs = 0;
            for (int f = 0; f < 7; ++f) {
                if ((passed & FileBBs[f]) && (passed & FileBBs[f+1])) ++pairs;
            }
            int sign = (c == WHITE) ? 1 : -1;
            mg += sign * pairs * params().ConnectedPasserMG;
            eg += sign * pairs * params().ConnectedPasserEG;
        }
    }

    // ---- Trade-down bonus (Round 33, EG only) ----
    // When materially ahead in PIECE value (non-pawn), prefer trading
    // remaining pieces to simplify. Applied as +/- per non-pawn piece on
    // the leading side, scaled by the leading side's lead magnitude.
    {
        Value wNP = pos.non_pawn_material(WHITE);
        Value bNP = pos.non_pawn_material(BLACK);
        if (wNP != bNP) {
            Color leader = (wNP > bNP) ? WHITE : BLACK;
            int sign = (leader == WHITE) ? 1 : -1;
            int leadPieces = popcount(pos.pieces(leader, KNIGHT))
                           + popcount(pos.pieces(leader, BISHOP))
                           + popcount(pos.pieces(leader, ROOK))
                           + popcount(pos.pieces(leader, QUEEN));
            // Fewer pieces on the leader's side = more simplified =
            // better. Bonus is inverse to piece count.
            eg += sign * (7 - std::min(7, leadPieces)) * params().TradeDownBonusEG;
        }
    }

    // ---- Rook trapped by own king (Round 35, MG only) ----
    // After loss of castling rights, a rook stuck in the corner with the
    // king blocking the e-side files is passive. CRITICAL: only fires
    // when castling RIGHTS on that side are LOST — otherwise normal
    // pre-castle positions (Ra1+Kc1, Rh1+Kg1) get false-flagged.
    {
        const CastlingRights kingside[2]  = { WHITE_OO,  BLACK_OO  };
        const CastlingRights queenside[2] = { WHITE_OOO, BLACK_OOO };
        for (Color c : { WHITE, BLACK }) {
            int sign = (c == WHITE) ? 1 : -1;
            Rank rank1 = (c == WHITE) ? RANK_1 : RANK_8;
            Bitboard rooksOnRank1 = pos.pieces(c, ROOK) & rank_bb(rank1);
            Square kSq = ksq[c];
            if (rank_of(kSq) != rank1) continue;
            int kingFile = int(file_of(kSq));
            bool canCastleK = pos.can_castle(kingside[c]);
            bool canCastleQ = pos.can_castle(queenside[c]);
            while (rooksOnRank1) {
                Square rSq = pop_lsb(rooksOnRank1);
                int rookFile = int(file_of(rSq));
                // Kingside trap: rook on files f-h, king on g/h-side (blocks
                // rook's exit toward e), no kingside castle right.
                if (!canCastleK
                    && rookFile >= 5
                    && kingFile >= 4 && kingFile <= rookFile
                    && kingFile > 3) {
                    mg -= sign * params().RookTrappedByKingMG;
                }
                // Queenside trap: rook on files a-c, king on a/b-side, no
                // queenside castle right.
                else if (!canCastleQ
                    && rookFile <= 2
                    && kingFile <= 3 && kingFile >= rookFile
                    && kingFile < 4) {
                    mg -= sign * params().RookTrappedByKingMG;
                }
            }
        }
    }

    // ---- Backward pawn on half-open file (Round 36) ----
    // A backward pawn on a file with no enemy pawn ahead is easy prey
    // for the enemy's rook. Extra penalty beyond R1's BackwardPawnPenalty.
    {
        for (Color c : { WHITE, BLACK }) {
            int sign = (c == WHITE) ? 1 : -1;
            Bitboard ourPawns   = pos.pieces(c, PAWN);
            Bitboard theirPawns = pos.pieces(~c, PAWN);
            Bitboard backward = backward_pawns(c, ourPawns,
                pawn_attacks_color(~c, theirPawns));
            Bitboard b = backward;
            while (b) {
                Square s = pop_lsb(b);
                File f = file_of(s);
                // Half-open file from c's POV: no enemy pawn on the same file
                // ahead of s.
                Bitboard ahead = (c == WHITE)
                    ? ~((Bitboard(1) << ((rank_of(s) + 1) * 8)) - 1)
                    : ((Bitboard(1) << (rank_of(s) * 8)) - 1);
                bool halfOpen = !(theirPawns & FileBBs[f] & ahead);
                if (halfOpen) {
                    mg -= sign * params().BackwardOnHalfOpenMG;
                    eg -= sign * params().BackwardOnHalfOpenEG;
                }
            }
        }
    }

    // ---- Mop-up eval (Round 20, EG only) ----
    // When one side has K (or K + 1 minor, no pawns) and the other side
    // has more, reward driving the loser's king to the corner and the
    // winner's king close. Helps mate conversion. EG only.
    {
        auto loner_side = [&](Color c) -> bool {
            return pos.pieces(c, PAWN) == 0
                && pos.pieces(c, ROOK) == 0
                && pos.pieces(c, QUEEN) == 0
                && popcount(pos.pieces(c, KNIGHT) | pos.pieces(c, BISHOP)) <= 1;
        };
        bool wIsLoner = loner_side(WHITE);
        bool bIsLoner = loner_side(BLACK);
        // Only trigger when exactly one side is a loner.
        if (wIsLoner != bIsLoner) {
            Color loser  = wIsLoner ? WHITE : BLACK;
            Color winner = ~loser;
            int sign = (winner == WHITE) ? 1 : -1;
            Square lk = ksq[loser];
            Square wk = ksq[winner];
            // Chebyshev distance from loser-king to nearest centre square.
            int dCentre = int(distance(lk, SQ_D4));
            dCentre = std::min(dCentre, int(distance(lk, SQ_E4)));
            dCentre = std::min(dCentre, int(distance(lk, SQ_D5)));
            dCentre = std::min(dCentre, int(distance(lk, SQ_E5)));
            int dKings = int(distance(wk, lk));
            int bonus = dCentre * params().MopUpKingCenter
                      + (14 - dKings) * params().MopUpKingDistance;
            eg += sign * bonus;
        }
    }

    // ---- Opposite-colour bishop endgame scaling (Round 21) ----
    // Each side has exactly 1 bishop on opposite colours, no other
    // minor/major pieces → strong drawing tendency, scale eg by
    // OCBEgScale% (default 50).
    {
        bool oneBishopEach =
            popcount(pos.pieces(WHITE, BISHOP)) == 1
            && popcount(pos.pieces(BLACK, BISHOP)) == 1
            && pos.pieces(KNIGHT) == 0
            && pos.pieces(ROOK)   == 0
            && pos.pieces(QUEEN)  == 0;
        if (oneBishopEach) {
            Bitboard wbBB = pos.pieces(WHITE, BISHOP);
            Bitboard bbBB = pos.pieces(BLACK, BISHOP);
            Square wb = pop_lsb(wbBB);
            Square bb = pop_lsb(bbBB);
            bool wbDark = (square_bb(wb) & DarkSquares) != 0;
            bool bbDark = (square_bb(bb) & DarkSquares) != 0;
            if (wbDark != bbDark) {
                eg = eg * params().OCBEgScale / 100;
            }
        }
    }

    // ---- Initiative bonus (Round 22, EG only) ----
    // Source: Stockfish 7 / sf_10 src/evaluate.cpp::initiative().
    // Endgame-only sign-aware complexity bonus that rewards positions
    // where the eg score is real (asymmetric pawns, both flanks, king
    // outflanking, pure endgame). The bonus is clamped to ±|eg| so it
    // can never flip the sign of the evaluation.
    {
        constexpr Bitboard QueenSideBB = FileABB | FileBBB | FileCBB | FileDBB;
        constexpr Bitboard KingSideBB  = FileEBB | FileFBB | FileGBB | FileHBB;
        int outflanking = std::abs(int(file_of(ksq[WHITE])) - int(file_of(ksq[BLACK])))
                        - std::abs(int(rank_of(ksq[WHITE])) - int(rank_of(ksq[BLACK])));
        Bitboard allPawns = pos.pieces(PAWN);
        bool pawnsBothFlanks = (allPawns & QueenSideBB) && (allPawns & KingSideBB);
        int totalPawns = popcount(allPawns);
        bool pureEndgame = (pos.non_pawn_material() == 0);
        int complexity = params().InitiativeOutflanking * outflanking
                       + params().InitiativePawnCount   * totalPawns
                       + params().InitiativeBothFlanks  * (pawnsBothFlanks ? 1 : 0)
                       + params().InitiativePureEndgame * (pureEndgame ? 1 : 0)
                       - params().InitiativeOffset;
        int sign_eg = (eg > 0) - (eg < 0);
        int initiative = sign_eg * std::max(complexity, -std::abs(eg));
        eg += initiative * params().InitiativeScale / 100;
    }

    // ---- KBNK mating drive (Round 23, EG only) ----
    // King + Bishop + Knight vs lone King is a known forced mate, but
    // only into the corner matching the bishop's colour. Without this
    // table, the search wanders for many moves before finding the
    // win — pushing toward the wrong corner draws by the 50-move rule.
    // Source: SF sf_10 src/endgame.cpp::Endgame<KBNK>.
    {
        auto bareKing = [&](Color c) -> bool {
            return pos.pieces(c, PAWN)   == 0
                && pos.pieces(c, KNIGHT) == 0
                && pos.pieces(c, BISHOP) == 0
                && pos.pieces(c, ROOK)   == 0
                && pos.pieces(c, QUEEN)  == 0;
        };
        auto hasBN = [&](Color c) -> bool {
            return pos.pieces(c, PAWN) == 0
                && pos.pieces(c, ROOK) == 0
                && pos.pieces(c, QUEEN) == 0
                && popcount(pos.pieces(c, BISHOP)) == 1
                && popcount(pos.pieces(c, KNIGHT)) == 1;
        };
        // SF's tables, exact values. PushClose: 0,0,100,80,60,40,20,10.
        // PushToCorners: 200 in matching corners (a1,h8), falling toward 110
        // at the off-colour corners (h1,a8). Halved here vs SF's 200-peak so
        // we don't dwarf material — this is supplemental, not standalone.
        static constexpr int PushClose[8] = { 0, 0, 100, 80, 60, 40, 20, 10 };
        static constexpr int PushToCorners[64] = {
            200, 190, 180, 170, 160, 150, 140, 130,
            190, 180, 170, 160, 150, 140, 130, 140,
            180, 170, 155, 140, 140, 125, 140, 150,
            170, 160, 140, 120, 110, 140, 150, 160,
            160, 150, 140, 110, 120, 140, 160, 170,
            150, 140, 125, 140, 140, 155, 170, 180,
            140, 130, 140, 150, 160, 170, 180, 190,
            130, 140, 150, 160, 170, 180, 190, 200
        };
        Color strong = (hasBN(WHITE) && bareKing(BLACK)) ? WHITE
                     : (hasBN(BLACK) && bareKing(WHITE)) ? BLACK
                     : COLOR_NB;
        if (strong != COLOR_NB) {
            Color weak = ~strong;
            Bitboard bishopsBB = pos.pieces(strong, BISHOP);
            Square bishopSq = pop_lsb(bishopsBB);
            Square winK = ksq[strong];
            Square losK = ksq[weak];
            // If the bishop is on a LIGHT square (a1 is dark), flip
            // squares so the table's "200 corners" become the light
            // corners (h1, a8) which are now drive-targets.
            bool bishopOnDark = (square_bb(bishopSq) & DarkSquares) != 0;
            if (!bishopOnDark) {
                winK = Square(winK ^ 56);   // mirror over a1-h8 (rank flip)
                losK = Square(losK ^ 56);
            }
            int bonus = PushToCorners[losK] * params().KBNKCornerScale / 100
                      + PushClose[distance(winK, losK)] * params().KBNKCloseScale / 100;
            int sign = (strong == WHITE) ? 1 : -1;
            eg += sign * bonus;
        }
    }

    // ---- Drawish endgame scaling (Round 25, EG only) ----
    // Recognize specific known-drawn material configurations and scale the
    // eg score down. Stockfish's scale_factor pattern, simplified to the
    // 3 most-impactful cases. The OCB scaling (R21) above is the first
    // case; this block adds:
    //   (a) Wrong-coloured bishop + only rook-pawn(s) vs bare king
    //       (defender king on/near queening square) → near-draw.
    //   (b) Single knight + only rook-pawn(s) vs bare king → drawish.
    //   (c) Generic rook-pair-or-fewer + few pawns ending → mild scale.
    // Apply ONLY when one side is materially stronger; for equal material
    // this would be a no-op since eg ≈ 0.
    {
        // Constants for "winning side" detection.
        auto strongerSide = [&]() -> Color {
            Value w = pos.non_pawn_material(WHITE);
            Value b = pos.non_pawn_material(BLACK);
            if (w > b + PieceValueMG[PAWN] / 2) return WHITE;
            if (b > w + PieceValueMG[PAWN] / 2) return BLACK;
            return COLOR_NB;
        };

        // (a) Wrong-bishop + only rook-pawn(s) check.
        // Bishop on file a/h is "wrong" if it cannot defend the queening
        // square. queening square = a8 / h8 (for white pawns).
        // Detect: strong has 1 bishop + only pawns on a/h file, no others.
        auto wrongBishopRookPawn = [&](Color strong) -> bool {
            Color weak = ~strong;
            if (popcount(pos.pieces(strong, BISHOP)) != 1) return false;
            if (pos.pieces(strong, KNIGHT)) return false;
            if (pos.pieces(strong, ROOK))   return false;
            if (pos.pieces(strong, QUEEN))  return false;
            // Weak side has at most a king (lone king required for the
            // strict draw rule).
            if (pos.pieces(weak, PAWN) || pos.pieces(weak, KNIGHT)
                || pos.pieces(weak, BISHOP) || pos.pieces(weak, ROOK)
                || pos.pieces(weak, QUEEN))
                return false;
            // Strong's pawns: only on file a OR only on file h.
            Bitboard sp = pos.pieces(strong, PAWN);
            if (!sp) return false;
            bool allA = !(sp & ~FileABB);
            bool allH = !(sp & ~FileHBB);
            if (!allA && !allH) return false;
            // Bishop colour must NOT match the promotion-square colour.
            // Promotion squares: a8 is LIGHT (the a1-h8 main diagonal is
            // dark, so a8 = off-corner = light). h8 is DARK.
            Bitboard wb = pos.pieces(strong, BISHOP);
            Square bishop = pop_lsb(wb);
            bool bishopOnDark = (square_bb(bishop) & DarkSquares) != 0;
            // "Wrong" bishop:
            //   a-pawns promote to a8 (light) → wrong bishop = dark-square.
            //   h-pawns promote to h8 (dark)  → wrong bishop = light-square.
            bool wrongBishop = allA ? bishopOnDark : !bishopOnDark;
            if (!wrongBishop) return false;
            // Weak king must be near the queening square (chebyshev ≤ 1
            // from the corner = sealed). Approximate: file matches.
            Square wkSq = ksq[weak];
            File queenFile = allA ? FILE_A : FILE_H;
            return file_of(wkSq) == queenFile
                || std::abs(int(file_of(wkSq)) - int(queenFile)) == 1;
        };

        // (b) Knight + only rook-pawn(s) vs bare king. Knight is too slow
        // to support the rook-pawn against a defending king in the corner.
        auto knightRookPawn = [&](Color strong) -> bool {
            Color weak = ~strong;
            if (popcount(pos.pieces(strong, KNIGHT)) != 1) return false;
            if (pos.pieces(strong, BISHOP)) return false;
            if (pos.pieces(strong, ROOK))   return false;
            if (pos.pieces(strong, QUEEN))  return false;
            if (pos.pieces(weak, PAWN) || pos.pieces(weak, KNIGHT)
                || pos.pieces(weak, BISHOP) || pos.pieces(weak, ROOK)
                || pos.pieces(weak, QUEEN))
                return false;
            Bitboard sp = pos.pieces(strong, PAWN);
            if (!sp) return false;
            bool allA = !(sp & ~FileABB);
            bool allH = !(sp & ~FileHBB);
            if (!allA && !allH) return false;
            // Weak king on the right corner side (file a or h).
            Square wkSq = ksq[weak];
            File queenFile = allA ? FILE_A : FILE_H;
            return file_of(wkSq) == queenFile
                || std::abs(int(file_of(wkSq)) - int(queenFile)) == 1;
        };

        Color str = strongerSide();
        if (str != COLOR_NB) {
            if (wrongBishopRookPawn(str)) {
                eg = eg * params().WrongBishopRPScale / 100;
            } else if (knightRookPawn(str)) {
                eg = eg * params().KnightRPScale / 100;
            }
        }
    }

    // ---- KPK bitbase probe (Round 26, EG only) ----
    // K + 1 pawn vs K is fully solvable. Probe the precomputed bitbase to
    // get the exact win/draw result; if drawn, scale eg down. This catches
    // the common "race to the queening square" cases the regular passed-
    // pawn eval misjudges (the defender king is just in time / just too
    // late).
    {
        bool kpvk =
            popcount(pos.pieces(WHITE, PAWN))   == 1
            && pos.pieces(BLACK, PAWN)   == 0
            && pos.pieces(WHITE, KNIGHT) == 0
            && pos.pieces(WHITE, BISHOP) == 0
            && pos.pieces(WHITE, ROOK)   == 0
            && pos.pieces(WHITE, QUEEN)  == 0
            && pos.pieces(BLACK, KNIGHT) == 0
            && pos.pieces(BLACK, BISHOP) == 0
            && pos.pieces(BLACK, ROOK)   == 0
            && pos.pieces(BLACK, QUEEN)  == 0;
        bool kpvk_inv =
            popcount(pos.pieces(BLACK, PAWN))   == 1
            && pos.pieces(WHITE, PAWN)   == 0
            && pos.pieces(WHITE, KNIGHT) == 0
            && pos.pieces(WHITE, BISHOP) == 0
            && pos.pieces(WHITE, ROOK)   == 0
            && pos.pieces(WHITE, QUEEN)  == 0
            && pos.pieces(BLACK, KNIGHT) == 0
            && pos.pieces(BLACK, BISHOP) == 0
            && pos.pieces(BLACK, ROOK)   == 0
            && pos.pieces(BLACK, QUEEN)  == 0;
        if (kpvk) {
            Bitboard pawnBB = pos.pieces(WHITE, PAWN);
            Square wp = pop_lsb(pawnBB);
            bool won = KPK::probe(ksq[WHITE], wp, ksq[BLACK],
                                  pos.side_to_move());
            if (!won) eg = eg * params().KPKDrawScale / 100;
        } else if (kpvk_inv) {
            // Black is strong side. Mirror the position vertically so KPK
            // probe (which expects white-as-strong-side) works.
            Bitboard pawnBB = pos.pieces(BLACK, PAWN);
            Square bp = pop_lsb(pawnBB);
            // Flip ranks: rank 1 <-> rank 8, etc.
            Square wpMirror = Square(int(bp) ^ 56);
            Square wkMirror = Square(int(ksq[BLACK]) ^ 56);
            Square bkMirror = Square(int(ksq[WHITE]) ^ 56);
            Color  stmMirror = ~pos.side_to_move();
            bool won = KPK::probe(wkMirror, wpMirror, bkMirror, stmMirror);
            if (!won) eg = eg * params().KPKDrawScale / 100;
        }
    }

    phase = std::min(phase, MaxPhase);
    int score = (mg * phase + eg * (MaxPhase - phase)) / MaxPhase;

    // Tapered tempo (Round 2): MG/EG split, Texel-tunable in eval_params.h.
    int tempo = (params().TempoMG * phase
               + params().TempoEG * (MaxPhase - phase)) / MaxPhase;

    return Value((pos.side_to_move() == WHITE ? score : -score) + tempo);
}

}  // namespace hypersion::Eval
