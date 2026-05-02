// Hypersion — Polyglot opening book reader.
//
// Polyglot books are 16-byte entries, big-endian, sorted by Zobrist key:
//   uint64 key
//   uint16 move (from/to/promotion encoded Polyglot-style — different from our Move)
//   uint16 weight (relative selection weight)
//   uint32 learn (unused)
//
// The Polyglot Zobrist scheme differs from ours, so we maintain a parallel
// hash for book lookups.

#ifndef HYPERSION_BOOK_H
#define HYPERSION_BOOK_H

#include <string>

#include "types.h"

namespace hypersion {
class Position;
}

namespace hypersion::Book {

bool is_open();
bool open(const std::string& path);
void close();

// Probes the book for the current position. Returns Move::none() if not found
// or no book is loaded. `pickBest`: choose highest-weight move (else weighted-random).
Move probe(const Position& pos, bool pickBest = false);

// Polyglot Zobrist key for a position (different from our internal key).
std::uint64_t polyglot_key(const Position& pos);

}  // namespace hypersion::Book

#endif  // HYPERSION_BOOK_H
