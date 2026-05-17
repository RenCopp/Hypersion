// Hypersion — pseudo-legal move generation, then a final legal-filter pass.
// Pawn moves, sliding-piece moves and king moves are generated separately;
// EVASIONS computes a custom target mask so the same helpers stay usable
// regardless of the in-check state.

#include "movegen.h"

#include <algorithm>     // std::min/max with initializer_list for chess960 castling

#include "bitboard.h"

namespace hypersion {

namespace {

// Emit promotion variants for a pawn that just landed on `to`.
// 2026-05-17 movegen audit: SF18 (movegen.cpp:108-124) splits promotions by
// capture-vs-push: in CAPTURES mode capture-promotions emit Q+R+B+N (rare
// but tactical — e.g. knight-capture-promotion delivering smothered mate),
// while push-promotions emit Q only. Previously Hypersion emitted Q only
// for ALL promotions in CAPTURES mode, losing capture-underpromotions in
// qsearch. The `Enemy` template parameter distinguishes the two cases.
template<GenType T, bool Enemy>
ExtMove* make_promotions(ExtMove* moveList, Square from, Square to) {
    constexpr bool all = (T == EVASIONS || T == NON_EVASIONS || T == LEGAL);
    if constexpr (T == CAPTURES || all)
        *moveList++ = Move::make(from, to, MT_PROMOTION, QUEEN);
    if constexpr ((T == CAPTURES && Enemy) || (T == QUIETS && !Enemy) || all) {
        *moveList++ = Move::make(from, to, MT_PROMOTION, ROOK);
        *moveList++ = Move::make(from, to, MT_PROMOTION, BISHOP);
        *moveList++ = Move::make(from, to, MT_PROMOTION, KNIGHT);
    }
    return moveList;
}

// All pawn moves for color Us. `target` restricts where landing squares may be
// (used by EVASIONS: only blocking squares + the checker's square).
template<Color Us, GenType T>
ExtMove* generate_pawn_moves(const Position& pos, ExtMove* moveList, Bitboard target) {
    constexpr Color     Them   = ~Us;
    constexpr Bitboard  TRank7 = (Us == WHITE ? Rank7BB : Rank2BB);
    constexpr Bitboard  TRank3 = (Us == WHITE ? Rank3BB : Rank6BB);
    constexpr Direction Up     = (Us == WHITE ? NORTH : SOUTH);
    constexpr Direction UpRight= (Us == WHITE ? NORTH_EAST : SOUTH_WEST);
    constexpr Direction UpLeft = (Us == WHITE ? NORTH_WEST : SOUTH_EAST);

    Bitboard ownPawns       = pos.pieces(Us, PAWN);
    Bitboard pawnsOn7       = ownPawns &  TRank7;
    Bitboard pawnsNotOn7    = ownPawns & ~TRank7;
    Bitboard enemies        = (T == EVASIONS) ? pos.checkers() : pos.pieces(Them);
    Bitboard emptySquares   = ~pos.pieces();

    // ---- Single + double pushes (quiets) ----
    if constexpr (T != CAPTURES) {
        Bitboard b1 = shift<Up>(pawnsNotOn7) & emptySquares;
        Bitboard b2 = shift<Up>(b1 & TRank3) & emptySquares;

        if constexpr (T == EVASIONS) {
            b1 &= target;
            b2 &= target;
        }
        while (b1) { Square to = pop_lsb(b1); *moveList++ = Move(Square(to - Up), to); }
        while (b2) { Square to = pop_lsb(b2); *moveList++ = Move(Square(to - Up - Up), to); }
    }

    // ---- Promotions (with or without capture) ----
    if (pawnsOn7) {
        Bitboard b1 = shift<UpRight>(pawnsOn7) & enemies;
        Bitboard b2 = shift<UpLeft >(pawnsOn7) & enemies;
        Bitboard b3 = shift<Up     >(pawnsOn7) & emptySquares;
        if constexpr (T == EVASIONS) b3 &= target;

        // b1, b2 are capture-promotions (Enemy=true); b3 is push-promotion (Enemy=false).
        while (b1) { Square to = pop_lsb(b1); moveList = make_promotions<T, true >(moveList, Square(to - UpRight), to); }
        while (b2) { Square to = pop_lsb(b2); moveList = make_promotions<T, true >(moveList, Square(to - UpLeft ), to); }
        while (b3) { Square to = pop_lsb(b3); moveList = make_promotions<T, false>(moveList, Square(to - Up     ), to); }
    }

    // ---- Standard captures (incl. en passant) ----
    if constexpr (T != QUIETS) {
        Bitboard b1 = shift<UpRight>(pawnsNotOn7) & enemies;
        Bitboard b2 = shift<UpLeft >(pawnsNotOn7) & enemies;
        while (b1) { Square to = pop_lsb(b1); *moveList++ = Move(Square(to - UpRight), to); }
        while (b2) { Square to = pop_lsb(b2); *moveList++ = Move(Square(to - UpLeft ), to); }

        if (Square ep = pos.ep_square(); ep != SQ_NONE) {
            // 2026-05-17 audit mg #9: unified the previous two constexpr
            // branches that computed the same `pawnsNotOn7 & pawn_attacks_bb`
            // pair. EVASIONS adds the gate "the captured pawn must be the
            // checker we're trying to resolve" — i.e., `ep - Up` (where the
            // doubly-pushed pawn now sits) must lie in our blocking target.
            bool epValid = true;
            if constexpr (T == EVASIONS)
                epValid = bool(target & square_bb(Square(ep - Up)));
            if (epValid) {
                Bitboard b = pawnsNotOn7 & pawn_attacks_bb(Them, ep);
                while (b) *moveList++ = Move::make(pop_lsb(b), ep, MT_EN_PASSANT);
            }
        }
    }
    return moveList;
}

template<Color Us, PieceType Pt>
ExtMove* generate_piece_moves(const Position& pos, ExtMove* moveList, Bitboard target) {
    static_assert(Pt != PAWN && Pt != KING);
    Bitboard b = pos.pieces(Us, Pt);
    while (b) {
        Square from = pop_lsb(b);
        Bitboard moves = attacks_bb<Pt>(from, pos.pieces()) & target;
        while (moves) *moveList++ = Move(from, pop_lsb(moves));
    }
    return moveList;
}

template<Color Us, GenType T>
ExtMove* generate_king_moves(const Position& pos, ExtMove* moveList) {
    Square ksq = pos.square<KING>(Us);
    Bitboard target = (T == CAPTURES) ? pos.pieces(~Us)
                    : (T == QUIETS)   ? ~pos.pieces()
                                      : ~pos.pieces(Us);
    Bitboard b = PseudoAttacks[KING][ksq] & target;
    while (b) *moveList++ = Move(ksq, pop_lsb(b));

    // ---- Castling (only in QUIETS or NON_EVASIONS) ----
    if constexpr (T == QUIETS || T == NON_EVASIONS) {
        if (pos.checkers()) return moveList;   // can't castle out of check

        for (CastlingRights cr : { (Us == WHITE ? WHITE_OO : BLACK_OO),
                                   (Us == WHITE ? WHITE_OOO : BLACK_OOO) }) {
            if (!pos.can_castle(cr)) continue;

            // Determine king destination and the actual rook square.
            // For Chess960 the rook may live on any file (not just H/A);
            // king destination is always G (kingside) or C (queenside) on
            // the home rank — same as standard chess.
            bool kingSide = (cr == WHITE_OO  || cr == BLACK_OO);
            Square kto    = make_square(kingSide ? FILE_G : FILE_C, Us == WHITE ? RANK_1 : RANK_8);
            Square rfrom  = pos.castling_rook_square(cr);
            Square rto    = make_square(kingSide ? FILE_F : FILE_D, Us == WHITE ? RANK_1 : RANK_8);

            // Squares between king's start/end AND rook's start/end must be
            // empty, ignoring the king and the castling-rook themselves
            // (in Chess960 the king may pass through the rook's square or
            // vice versa during castling; that's fine).
            Square lo = std::min({ksq, kto, rfrom, rto});
            Square hi = std::max({ksq, kto, rfrom, rto});
            bool blockedByPiece = false;
            for (Square s = lo; s <= hi; ++s) {
                if (s == ksq || s == rfrom) continue;
                if (pos.piece_on(s) != NO_PIECE) { blockedByPiece = true; break; }
            }
            if (blockedByPiece) continue;

            // Squares king passes through (inclusive of start and end) must
            // not be attacked by the opponent.
            bool kpAttacked = false;
            Square kLo = std::min(ksq, kto), kHi = std::max(ksq, kto);
            for (Square s = kLo; s <= kHi; ++s) {
                if (pos.attackers_to(s) & pos.pieces(~Us)) { kpAttacked = true; break; }
            }
            if (kpAttacked) continue;

            *moveList++ = Move::make(ksq, kto, MT_CASTLING);
        }
    }
    return moveList;
}

template<Color Us, GenType T>
ExtMove* generate_all(const Position& pos, ExtMove* moveList) {
    constexpr bool Evasion = (T == EVASIONS);

    Square ksq = pos.square<KING>(Us);

    Bitboard target;
    if constexpr (Evasion) {
        // In double check, only king moves are legal.
        if (more_than_one(pos.checkers())) return generate_king_moves<Us, EVASIONS>(pos, moveList);
        // Single checker: block on the line between king and checker, or capture the checker.
        Square checkerSq = lsb(pos.checkers());
        target = BetweenBB[ksq][checkerSq];
    } else if constexpr (T == NON_EVASIONS) target = ~pos.pieces(Us);
    else if constexpr (T == CAPTURES)      target =  pos.pieces(~Us);
    else /* QUIETS */                      target = ~pos.pieces();

    moveList = generate_pawn_moves <Us, T>     (pos, moveList, target);
    moveList = generate_piece_moves<Us, KNIGHT>(pos, moveList, target);
    moveList = generate_piece_moves<Us, BISHOP>(pos, moveList, target);
    moveList = generate_piece_moves<Us, ROOK>  (pos, moveList, target);
    moveList = generate_piece_moves<Us, QUEEN> (pos, moveList, target);

    moveList = generate_king_moves<Us, T>(pos, moveList);
    return moveList;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public generate<T> entry points
// ---------------------------------------------------------------------------
template<>
ExtMove* generate<CAPTURES>(const Position& pos, ExtMove* moveList) {
    return pos.side_to_move() == WHITE ? generate_all<WHITE, CAPTURES>(pos, moveList)
                                       : generate_all<BLACK, CAPTURES>(pos, moveList);
}
template<>
ExtMove* generate<QUIETS>(const Position& pos, ExtMove* moveList) {
    return pos.side_to_move() == WHITE ? generate_all<WHITE, QUIETS>(pos, moveList)
                                       : generate_all<BLACK, QUIETS>(pos, moveList);
}
template<>
ExtMove* generate<EVASIONS>(const Position& pos, ExtMove* moveList) {
    return pos.side_to_move() == WHITE ? generate_all<WHITE, EVASIONS>(pos, moveList)
                                       : generate_all<BLACK, EVASIONS>(pos, moveList);
}
template<>
ExtMove* generate<NON_EVASIONS>(const Position& pos, ExtMove* moveList) {
    return pos.side_to_move() == WHITE ? generate_all<WHITE, NON_EVASIONS>(pos, moveList)
                                       : generate_all<BLACK, NON_EVASIONS>(pos, moveList);
}
template<>
ExtMove* generate<LEGAL>(const Position& pos, ExtMove* moveList) {
    ExtMove* end = pos.checkers()
                 ? generate<EVASIONS>(pos, moveList)
                 : generate<NON_EVASIONS>(pos, moveList);

    // 2026-05-17 audit mg #4: SF18 src/movegen.cpp:293-310 skips the full
    // pos.legal() call for moves whose from-square isn't pinned, isn't the
    // king, and isn't an en-passant capture. Those moves are guaranteed
    // legal by the pseudo-legal generator; checking them is wasted work.
    // Pinned and king moves can leave the king in check; EN_PASSANT can
    // remove a discovered-check blocker. Saves ~10-15% on LEGAL gen time.
    Color us         = pos.side_to_move();
    Square ksq       = pos.square<KING>(us);
    Bitboard pinned  = pos.blockers_for_king(us) & pos.pieces(us);

    ExtMove* cur = moveList;
    while (cur != end) {
        Move m = Move(*cur);
        bool needsCheck = (pinned & square_bb(m.from_sq()))
                       || m.from_sq() == ksq
                       || m.type_of() == MT_EN_PASSANT;
        if (!needsCheck || pos.legal(m)) ++cur;
        else                              *cur = *(--end);
    }
    return end;
}

}  // namespace hypersion
