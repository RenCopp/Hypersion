// Hypersion — classical evaluator.
// Material + tapered PSQT + mobility + king safety + pawn structure + threats.

#include "evaluate.h"

#include "bitboard.h"
#include "eval_params.h"
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

void init() { /* nothing dynamic for now */ }

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
            while (b) {
                Square s = pop_lsb(b);
                int psqIdx = mirror(s, c);
                mg += sign * (PieceValueMG[pt] + PSQ_MG[pt][psqIdx]);
                eg += sign * (PieceValueEG[pt] + PSQ_EG[pt][psqIdx]);

                // ---- Mobility & king-attack tracking for sliders / knights ----
                if (pt == KNIGHT || pt == BISHOP || pt == ROOK || pt == QUEEN) {
                    Bitboard atk = (pt == KNIGHT) ? PseudoAttacks[KNIGHT][s]
                                 : (pt == BISHOP) ? attacks_bb<BISHOP>(s, occupied)
                                 : (pt == ROOK)   ? attacks_bb<ROOK>  (s, occupied)
                                                  : attacks_bb<QUEEN> (s, occupied);
                    int mobCount = popcount(atk & mobilityArea);
                    if (mobCount > 27) mobCount = 27;
                    mg += sign * MobilityMG[pt][mobCount];
                    eg += sign * MobilityEG[pt][mobCount];

                    // King-attack contribution (attacks on enemy king zone).
                    Bitboard kingAttacks = atk & kingZone[them];
                    if (kingAttacks) {
                        attackUnits[c]   += KingAttackerWeight[pt] * popcount(kingAttacks);
                        ++attackerCount[c];
                    }

                    // Safe-check contribution. Adds to attackUnits but does
                    // NOT increment attackerCount — that gating remains for
                    // king-zone attackers only.
                    Bitboard safeChecks = atk & kingChecksBy[pt] & ~themDefended;
                    if (safeChecks)
                        attackUnits[c] += SafeCheckWeight[pt] * popcount(safeChecks);

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
                }
            }
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
                mg += sign * ConnectedPawnBonus[relRank] / 2;
                eg += sign * ConnectedPawnBonus[relRank];
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
            for (PieceType vt = KNIGHT; vt <= QUEEN; vt = PieceType(vt + 1)) {
                Bitboard victims = pos.pieces(them, vt);
                int n1 = popcount(victims & minorAtkAll);
                int n2 = popcount(victims & rookAtkAll);
                int n3 = popcount(victims & pawnAtkAll);
                mg += sign * n1 * ThreatByMinorMG[vt];
                mg += sign * n2 * ThreatByRookMG [vt];
                mg += sign * n3 * ThreatByPawnMG [vt];
                eg += sign * n3 * ThreatByPawnEG [vt];
            }

            // Hanging: enemy pieces attacked by us but not defended.
            Bitboard ourAttacks = minorAtkAll | rookAtkAll
                                | pawn_attacks_color(c, pawns[c]);
            Bitboard theirDef   = pawn_attacks_color(them, pawns[them])
                                | PseudoAttacks[KING][ksq[them]];
            // (rough approximation — full attack set would require per-piece scan)
            Bitboard hanging = pos.pieces(them) & ~pos.pieces(them, PAWN) & ~pos.pieces(them, KING)
                             & ourAttacks & ~theirDef;
            mg += sign * popcount(hanging) * params().HangingPenaltyMG;
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
            if (missing > 0) mg -= sign * PawnShelterMissingMG[std::min(missing, 3)];
        }

        // ---- Bishop pair bonus ----
        if (popcount(pos.pieces(c, BISHOP)) >= 2) {
            mg += sign * params().BishopPairBonusMG;
            eg += sign * params().BishopPairBonusEG;
        }

        // ---- Rook on open / semi-open file ----
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
                eg += sign * PassedRankBonus[relRank];
                mg += sign * PassedRankBonus[relRank] / 3;
            }
            // Isolated: no friendly pawn on adjacent files.
            if (!(adjacent_files_bb(file_of(s)) & pawns[c])) {
                mg -= sign * params().IsolatedPawnPenalty;
                eg -= sign * params().IsolatedPawnPenalty;
            }
            // Doubled: another friendly pawn on same file behind.
            if (popcount(pawns[c] & FileBBs[file_of(s)]) > 1) {
                mg -= sign * params().DoubledPawnPenalty / 2;   // shared between the two
                eg -= sign * params().DoubledPawnPenalty / 2;
            }
            // Backward: stop square attacked by enemy pawn, no friendly
            // pawn on adjacent file at our rank or behind to support a
            // safe advance.
            if (backward & square_bb(s)) {
                mg -= sign * params().BackwardPawnPenalty;
                eg -= sign * params().BackwardPawnPenalty;
            }
        }
    }

    // ---- King safety: scale attack units through SafetyMargin curve. ----
    for (Color c : { WHITE, BLACK }) {
        int sign = (c == WHITE) ? 1 : -1;
        if (attackerCount[c] >= 2) {
            int idx = std::min(99, attackUnits[c] / 8);
            mg += sign * SafetyMargin[idx];
        }
    }

    phase = std::min(phase, MaxPhase);
    int score = (mg * phase + eg * (MaxPhase - phase)) / MaxPhase;

    return Value((pos.side_to_move() == WHITE ? score : -score) + Tempo);
}

}  // namespace hypersion::Eval
