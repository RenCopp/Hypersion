// Hypersion — move generation.
// Templated on a GenType so the search can ask for just captures, just quiets,
// only evasions while in check, or every legal move. Output is written to a
// caller-provided ExtMove[] buffer; generate() returns a pointer one past the
// last move written (STL-style).

#ifndef HYPERSION_MOVEGEN_H
#define HYPERSION_MOVEGEN_H

#include "position.h"
#include "types.h"

namespace hypersion {

enum GenType {
    CAPTURES,        // captures (incl. en passant) and ALL promotions
    QUIETS,          // non-capture, non-promotion moves (incl. castling)
    EVASIONS,        // any move that gets out of check
    NON_EVASIONS,    // CAPTURES ∪ QUIETS  (used when not in check)
    LEGAL            // strictly legal moves (filtered)
};

// Move with an attached score slot (filled by movepicker / movesort).
struct ExtMove {
    Move move;
    int  value;

    operator Move() const { return move; }
    void operator=(Move m) { move = m; }
};

inline bool operator<(const ExtMove& a, const ExtMove& b) { return a.value < b.value; }

template<GenType T>
ExtMove* generate(const Position& pos, ExtMove* moveList);

// MoveList<T> — STL-friendly wrapper. Use:
//   for (auto m : MoveList<LEGAL>(pos)) ...
template<GenType T>
class MoveList {
    ExtMove  list[MAX_MOVES];
    ExtMove* last;
public:
    explicit MoveList(const Position& pos) : last(generate<T>(pos, list)) {}
    const ExtMove* begin() const { return list; }
    const ExtMove* end()   const { return last; }
    size_t         size()  const { return size_t(last - list); }
    bool           contains(Move m) const {
        for (auto p = begin(); p != end(); ++p) if (Move(*p) == m) return true;
        return false;
    }
};

}  // namespace hypersion

#endif  // HYPERSION_MOVEGEN_H
