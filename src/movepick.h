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
    // Main search.
    // 2026-05-17 audit #6.3: split QUIET into GOOD_QUIET / BAD_QUIET
    // around a depth-scaled threshold, mirroring SF18 movepick.cpp:
    // 39-41. After GOOD_QUIET returns moves with value above the
    // threshold, BAD_CAPTURE drains, then BAD_QUIET returns the
    // remaining low-history quiets.
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, KILLER0, KILLER1, QUIET_INIT,
    GOOD_QUIET, BAD_CAPTURE, BAD_QUIET,
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
               Piece            prevPiece2= NO_PIECE,
               const ThreatSquareHistory* threatH = nullptr,
               int              threatSq  = 64);
    // 2026-05-19 T2 REJECT (+1.7 +/- 38.5 ELO @ 200g 5+0.05 — noise/no-ship):
    // tested Obsidian-style dedicated COUNTERMOVE stage between KILLER1 and
    // QUIET_INIT, with `counterMove` constructor param threaded from
    // Worker.counterMoves[prevPiece][prevTo] in search.cpp. Result was
    // statistically indistinguishable from baseline (64W-63L-73D). Likely
    // cause: Hypersion's 2-ply contHist (contHist1 + contHist2) read in
    // score_quiets already captures the counter-move signal — contHist1
    // IS the 1-ply-back counter-move history. The dedicated stage adds
    // ordering priority but no new INFORMATION, so it's neutral. SF18
    // arrived at the same conclusion and removed the explicit counter-move
    // stage; Obsidian/Berserk still ship it for legacy reasons. Future
    // contributors: do not re-test in isolation. Source consulted:
    // C:\Engine\Engines\Obsidian-16.0\Obsidian-16.0\src\movepick.cpp:177-183.

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
    // 2026-05-18 Tier 2: RubiChess threat-square HH read at score_quiets time.
    const ThreatSquareHistory* tsHist  = nullptr;
    int                        tsSq    = 64;
    // 2026-05-19 T3 REJECT: rootHist member removed; see history.h tombstone.

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
