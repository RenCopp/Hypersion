// Hypersion — MovePicker implementation.

#include "movepick.h"

#include <algorithm>

#include "bitboard.h"

namespace hypersion {

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
    // Quiet move ordering: butterfly history + 1-ply continuation history.
    // contHist2 (2-ply lookback) is UPDATED by search.cpp on cutoffs but is
    // intentionally NOT read here: experiments adding it (with both /2 and
    // /4 weights, plus 4-ply variants) regressed ~26 ELO. Re-tuning weights
    // is left for a future Texel-style sweep.
    bool useCont1 = contHist1 != nullptr
                 && prevPc != NO_PIECE
                 && prevMv != Move::none()
                 && prevMv != Move::null();
    for (auto* it = cur; it != endMoves; ++it) {
        Move m = it->move;
        int v = bhist ? bhist->get(pos.side_to_move(), m) : 0;
        Piece moving = pos.piece_on(m.from_sq());
        if (useCont1) v += contHist1->get(prevPc, prevMv.to_sq(), moving, m.to_sq());
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
