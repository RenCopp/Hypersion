// Hypersion — MovePicker implementation.

#include "movepick.h"

#include <algorithm>

#include "bitboard.h"
#include "evaluate.h"   // PieceValueMG[]

namespace hypersion {

// Forward decls into Search::tunables for A2 SPSA campaign.
// Definitions live in src/search.cpp::tunables. A2 SPSA-v2 shipped
// values 101/99/47 (vs pre-A2 fixed weights 100/100/50) worth
// +27.9 ELO @ 400g (combined of two independent 200g confirms).
// Runtime-tunable via `setoption name Tune_<NAME> value <int>`.
namespace Search::tunables {
extern int BFLY_WEIGHT;
extern int CONT1_WEIGHT;
extern int CONT2_WEIGHT;
}

namespace {
inline Value piece_value_simple(PieceType pt) {
    constexpr Value v[PIECE_TYPE_NB] = { 0, 100, 320, 330, 500, 900, 0, 0 };
    return v[pt];
}
}  // namespace

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------
MovePicker::MovePicker(const Position& p,
                       Move ttm,
                       const ButterflyHistory* bh,
                       const CaptureHistory*   ch,
                       const Move* killers,
                       int d,
                       const ContinuationHistory* contH,
                       Move pm,
                       Piece pp,
                       const ContinuationHistory* contH2,
                       Move pm2,
                       Piece pp2)
    : pos(p), bhist(bh), chist(ch),
      contHist1(contH), contHist2(contH2),
      ttMove(ttm),
      killer0(killers ? killers[0] : Move::none()),
      killer1(killers ? killers[1] : Move::none()),
      prevMv(pm),  prevMv2(pm2),
      prevPc(pp),  prevPc2(pp2),
      depth(d) {
    stage = pos.checkers() ? EVASION_TT : MAIN_TT;
    if (ttm != Move::none() && !pos.pseudo_legal(ttm)) ttMove = Move::none();
    if (ttMove == Move::none()) ++stage;   // skip *_TT stages
}

MovePicker::MovePicker(const Position& p,
                       Move ttm,
                       const ButterflyHistory* bh,
                       const CaptureHistory*   ch,
                       int qd)
    : pos(p), bhist(bh), chist(ch),
      ttMove(ttm), depth(qd) {
    // When in check, we must look at every evasion (not just captures), otherwise
    // we'd accept moves that don't resolve the check.
    if (pos.checkers()) {
        stage = EVASION_TT;
        if (ttm != Move::none() && !pos.pseudo_legal(ttm)) ttMove = Move::none();
    } else {
        stage = QSEARCH_TT;
        if (ttm != Move::none() && (!pos.pseudo_legal(ttm) || (qd <= 0 && !pos.capture(ttm))))
            ttMove = Move::none();
    }
    if (ttMove == Move::none()) ++stage;
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------
void MovePicker::score_captures() {
    for (auto* it = cur; it != endMoves; ++it) {
        Move m = it->move;
        PieceType victim = type_of(pos.piece_on(m.to_sq()));
        if (m.type_of() == MT_EN_PASSANT) victim = PAWN;
        Piece moving = pos.piece_on(m.from_sq());
        int   capHist = chist ? chist->get(moving, m.to_sq(), victim) : 0;
        it->value = 7 * int(piece_value_simple(victim)) + capHist;
    }
}

void MovePicker::score_quiets() {
    // Quiet move ordering: butterfly history + 1-ply continuation history,
    // PLUS Stockfish-18-style threat-aware bonus / penalty:
    //
    //   Penalty for moving a piece TO a square attacked by a LESSER piece
    //   (e.g. queen walking into a pawn-attacked square is terrible).
    //   Bonus for moving a piece AWAY from such a square (escape).
    //
    // Magnitudes use PieceValueMG[], so a queen escape bumps the score by
    // ~50 k while butterfly history caps at ~7 k — threat-driven moves
    // dominate when threats exist.  Source: SF18 movepick.cpp::score().
    Color us   = pos.side_to_move();
    Color them = ~us;

    // attacks_by_lesser[pt] = squares attacked by enemy pieces strictly
    // less valuable than `pt`.  Index 0 (NO_PIECE) and 7 (ALL_PIECES) unused.
    Bitboard atkBy_PAWN   = 0, atkBy_KNIGHT = 0, atkBy_BISHOP = 0, atkBy_ROOK = 0;
    {
        Bitboard oppPawns = pos.pieces(them, PAWN);
        atkBy_PAWN = (them == WHITE)
            ? pawn_attacks_bb<WHITE>(oppPawns)
            : pawn_attacks_bb<BLACK>(oppPawns);
        Bitboard occ = pos.pieces();
        Bitboard bb;
        bb = pos.pieces(them, KNIGHT);
        while (bb) atkBy_KNIGHT |= PseudoAttacks[KNIGHT][pop_lsb(bb)];
        bb = pos.pieces(them, BISHOP);
        while (bb) atkBy_BISHOP |= attacks_bb<BISHOP>(pop_lsb(bb), occ);
        bb = pos.pieces(them, ROOK);
        while (bb) atkBy_ROOK   |= attacks_bb<ROOK>  (pop_lsb(bb), occ);
        // Queen attacks contribute only to threatByLesser[KING]; we don't
        // bother since the king is never reordered by this heuristic in
        // practice (the king is the most-mover-of-last-resort move type).
    }
    Bitboard threatByLesser[PIECE_TYPE_NB] = {};
    threatByLesser[KNIGHT] = atkBy_PAWN;
    threatByLesser[BISHOP] = atkBy_PAWN;
    threatByLesser[ROOK]   = atkBy_PAWN | atkBy_KNIGHT | atkBy_BISHOP;
    threatByLesser[QUEEN]  = threatByLesser[ROOK] | atkBy_ROOK;

    bool useCont1 = contHist1 != nullptr
                 && prevPc != NO_PIECE
                 && prevMv != Move::none()
                 && prevMv != Move::null();
    bool useCont2 = contHist2 != nullptr
                 && prevPc2 != NO_PIECE
                 && prevMv2 != Move::none()
                 && prevMv2 != Move::null();
    // A2 SPSA campaign tunables (defaults 100, 100, 50 reproduce 1.0x,
    // 1.0x, 0.5x). Hoist out of the loop body so the divide-by-100 isn't
    // re-emitted per move.
    const int bflyW  = Search::tunables::BFLY_WEIGHT;
    const int cont1W = Search::tunables::CONT1_WEIGHT;
    const int cont2W = Search::tunables::CONT2_WEIGHT;

    for (auto* it = cur; it != endMoves; ++it) {
        Move m = it->move;
        int v = bhist ? (bhist->get(us, m) * bflyW / 100) : 0;
        Piece moving = pos.piece_on(m.from_sq());
        PieceType pt = type_of(moving);
        if (useCont1) v += contHist1->get(prevPc, prevMv.to_sq(), moving, m.to_sq()) * cont1W / 100;
        // Read 2-ply continuation history with SPSA-tuned weight (default 50%).
        // Pre-A2 fixed half-weight tested at +34.9 ELO @ 200g vs no-read.
        // Full weight tested at 0.0 +/- 38 ELO vs half weight (equivalent
        // by measurement); kept half weight as the conventional default,
        // now SPSA-tunable.
        if (useCont2) v += contHist2->get(prevPc2, prevMv2.to_sq(), moving, m.to_sq()) * cont2W / 100;

        // Threat-by-lesser bonus / penalty (SF18).
        // NOTE: tested SF18 check-bonus +16384 (after SEE >= -75 filter)
        // here. Result: -11.6 +/- 95.8 ELO @ 30g. Within noise but mildly
        // negative — Hypersion's threat-by-lesser already pulls forcing
        // moves earlier; the additional check bonus gave no clean win.
        // Keep it filed under "may need a smaller magnitude or wider
        // sample size" for later.
        if (pt >= KNIGHT && pt <= QUEEN) {
            Bitboard tBL  = threatByLesser[pt];
            Bitboard toBB = Bitboard(1) << int(m.to_sq());
            Bitboard frBB = Bitboard(1) << int(m.from_sq());
            int signedV   = (tBL & toBB) ? -19
                          : (tBL & frBB) ?  20
                                         :   0;
            v += int(Eval::PieceValueMG[pt]) * signedV;
        }

        it->value = v;
    }
}

void MovePicker::score_evasions() {
    for (auto* it = cur; it != endMoves; ++it) {
        Move m = it->move;
        if (pos.capture(m)) {
            PieceType victim = type_of(pos.piece_on(m.to_sq()));
            if (m.type_of() == MT_EN_PASSANT) victim = PAWN;
            it->value = int(piece_value_simple(victim)) + (1 << 28);   // captures dominate quiets
        } else {
            it->value = bhist ? bhist->get(pos.side_to_move(), m) : 0;
        }
    }
}

// Selection sort: pick the highest-scoring move from [begin, end), swap to begin.
ExtMove* MovePicker::best_at(ExtMove* begin, ExtMove* end) {
    auto* best = begin;
    for (auto* it = begin + 1; it != end; ++it)
        if (it->value > best->value) best = it;
    std::swap(*begin, *best);
    return begin;
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
Move MovePicker::next_move(bool skipQuiets) {
top:
    switch (stage) {

    case MAIN_TT:
    case EVASION_TT:
    case QSEARCH_TT:
        ++stage;
        return ttMove;

    case CAPTURE_INIT:
        cur = endMoves = movesBuf;
        endMoves = generate<CAPTURES>(pos, movesBuf);
        score_captures();
        ++stage;
        [[fallthrough]];
    case GOOD_CAPTURE:
        while (cur < endMoves) {
            best_at(cur, endMoves);
            ExtMove em = *cur++;
            if (em.move == ttMove) continue;
            // SEE-split: captures whose static exchange value is >= 0 are
            // "good" and returned now; the rest go to the bad-capture buffer
            // and come out after quiets.
            if (pos.see_ge(em.move, VALUE_ZERO)) return em.move;
            // Stash bad captures (with their score) for later.
            if (endBad - badCapBuf < 64) *endBad++ = em;
        }
        ++stage;
        [[fallthrough]];

    case KILLER0:
        ++stage;
        if (killer0 != Move::none() && killer0 != ttMove
            && !pos.capture(killer0) && pos.pseudo_legal(killer0))
            return killer0;
        [[fallthrough]];

    case KILLER1:
        ++stage;
        if (killer1 != Move::none() && killer1 != ttMove
            && !pos.capture(killer1) && pos.pseudo_legal(killer1))
            return killer1;
        [[fallthrough]];

    case QUIET_INIT:
        if (skipQuiets) { stage = BAD_CAPTURE; goto top; }
        cur = endMoves = movesBuf;
        endMoves = generate<QUIETS>(pos, movesBuf);
        score_quiets();
        // NOTE: tested std::stable_sort here for tie-determinism (Hypersion's
        // bench is non-deterministic across processes even with Threads=1,
        // unlike Stockfish; std::sort with strict-weak-only comparator gives
        // unspecified order on equal-score ties, varying with ASLR's effect
        // on introsort partitioning). Result: -56.1 +/- 54.3 ELO @ 100g
        // 5+0.05. The deterministic-tie order from stable_sort isn't worth
        // the per-call overhead. Bench non-determinism is upstream of move
        // ordering — leaving the search non-deterministic for now (game
        // SPRT still gives unbiased ELO since both sides are equally
        // affected). Future fix: identify the actual root cause of
        // non-determinism (NOT a movepick sort issue) and address that.
        std::sort(movesBuf, endMoves,
                  [](const ExtMove& a, const ExtMove& b) { return a.value > b.value; });
        ++stage;
        [[fallthrough]];
    case QUIET:
        while (cur < endMoves) {
            Move m = (cur++)->move;
            if (m == ttMove || m == killer0 || m == killer1) continue;
            return m;
        }
        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE:
        // Iterate the badCapBuf in score order via separate cursor (not movesBuf cur).
        while (badCur < endBad) {
            ExtMove* best = badCur;
            for (auto* it = badCur + 1; it != endBad; ++it)
                if (it->value > best->value) best = it;
            std::swap(*badCur, *best);
            return (badCur++)->move;
        }
        return Move::none();

    case EVASION_INIT:
        cur = endMoves = movesBuf;
        endMoves = generate<EVASIONS>(pos, movesBuf);
        score_evasions();
        // (See QUIET_INIT tombstone for stable_sort tradeoff.)
        std::sort(movesBuf, endMoves,
                  [](const ExtMove& a, const ExtMove& b) { return a.value > b.value; });
        ++stage;
        [[fallthrough]];
    case EVASION:
        while (cur < endMoves) {
            Move m = (cur++)->move;
            if (m == ttMove) continue;
            return m;
        }
        return Move::none();

    case QCAPTURE_INIT:
        cur = endMoves = movesBuf;
        endMoves = generate<CAPTURES>(pos, movesBuf);
        score_captures();
        ++stage;
        [[fallthrough]];
    case QCAPTURE:
        while (cur < endMoves) {
            best_at(cur, endMoves);
            Move m = (cur++)->move;
            if (m == ttMove) continue;
            return m;
        }
        return Move::none();
    }

    return Move::none();
}

}  // namespace hypersion
