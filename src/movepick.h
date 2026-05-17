// Hypersion — MovePicker.
//
// Lazy, staged move ordering. Search calls next_move() in a loop; the picker
// returns moves in a search-friendly order:
//
//    1) TT move
//    2) Winning captures (sorted by MVV/LVA + capture history)
//    3) Killers (two quiet moves that caused recent cutoffs at this ply)
//    4) Quiets (sorted by butterfly history)
//    5) Losing captures (sorted same as 2)
//
// Pseudo-legal moves only — search verifies legality before recursing.

#ifndef HYPERSION_MOVEPICK_H
#define HYPERSION_MOVEPICK_H

#include "history.h"
#include "movegen.h"
#include "position.h"
#include "types.h"

namespace hypersion {

enum Stage : std::uint8_t {
    // Main search
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, KILLER0, KILLER1, QUIET_INIT, QUIET, BAD_CAPTURE,
    // Evasions
    EVASION_TT, EVASION_INIT, EVASION,
    // Quiescence
    QSEARCH_TT, QCAPTURE_INIT, QCAPTURE
};
inline Stage& operator++(Stage& s) { return s = Stage(int(s) + 1); }

class MovePicker {
public:
    MovePicker(const Position&  p,
               Move             ttm,
               const ButterflyHistory* bh,
               const CaptureHistory*   ch,
               const Move*      killers,
               int              depth,
               const ContinuationHistory* contHist  = nullptr,
               Move             prevMove  = Move::none(),
               Piece            prevPiece = NO_PIECE,
               const ContinuationHistory* contHist2 = nullptr,
               Move             prevMove2 = Move::none(),
               Piece            prevPiece2= NO_PIECE);

    // Quiescence-only constructor (no killers, no quiets).
    // 2026-05-17 audit qs #18: now accepts contHist + prev-ply info so
    // evasion-move ordering inside qsearch can use the parent's
    // continuation history (matching SF18 — qsearch evasions otherwise
    // get only mainHist + captureHist signal, missing contHist gradient).
    MovePicker(const Position&  p,
               Move             ttm,
               const ButterflyHistory* bh,
               const CaptureHistory*   ch,
               int              qDepth,
               const ContinuationHistory* contHist  = nullptr,
               Move             prevMove  = Move::none(),
               Piece            prevPiece = NO_PIECE,
               const ContinuationHistory* contHist2 = nullptr,
               Move             prevMove2 = Move::none(),
               Piece            prevPiece2= NO_PIECE);

    Move next_move(bool skipQuiets = false);

private:
    void score_captures();
    void score_quiets();
    void score_evasions();
    ExtMove* best_at(ExtMove* begin, ExtMove* end);

    const Position&            pos;
    const ButterflyHistory*    bhist;
    const CaptureHistory*      chist;
    const ContinuationHistory* contHist1 = nullptr;   // 1-ply back (counter-move)
    const ContinuationHistory* contHist2 = nullptr;   // 2-ply back (follow-up; updated, not read)

    Move    ttMove;
    Move    killer0;
    Move    killer1;
    // 2026-05-16 default-init: previously these were left uninitialized
    // in the qsearch MovePicker constructor (latent garbage). Now safe
    // for score_evasions to read contHist1 in qsearch too (Finding 5 of
    // the SF-diff audit — landmine if any future code reaches them
    // without parent-ply info).
    Move    prevMv  = Move::none(), prevMv2 = Move::none();
    Piece   prevPc  = NO_PIECE,     prevPc2 = NO_PIECE;
    int     depth;
    Stage   stage;

    ExtMove movesBuf[MAX_MOVES];
    ExtMove badCapBuf[64];   // up to 64 bad captures retained for stage BAD_CAPTURE
    ExtMove* cur     = movesBuf;
    ExtMove* endMoves= movesBuf;
    ExtMove* badCur  = badCapBuf;
    ExtMove* endBad  = badCapBuf;
};

}  // namespace hypersion

#endif  // HYPERSION_MOVEPICK_H
