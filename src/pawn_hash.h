// Hypersion — pawn-structure evaluation cache (R38).
//
// Thread-local hash table keyed by `Position::pawn_key()` that caches the
// mg/eg contribution of pawn-structure-only evaluation terms PLUS the
// per-side passed-pawn bitboards (needed by king-distance + storm terms
// that depend on the live king position and therefore CAN'T themselves
// be cached).
//
// Hit rate: typically 95-99 % since pawn structure changes only on pawn
// moves/captures (rare in tree). Reference: chessprogramming.org/
// Pawn_Hash_Table.
//
// What IS cached:
//   * Pure-pawn evaluations (isolated, doubled, backward, connected
//     phalanx, pawn islands, pawn lever, candidate-passed)
//   * Passed-pawn detection bitboard per side (so the main eval can
//     iterate them again for king-distance / storm scoring without
//     re-doing the passed_pawn_span check)
//
// What is NOT cached (depends on king or pieces, not pawn-only):
//   * King-zone pawn shelter
//   * Passed-pawn × king-distance bonuses
//   * Pawn storm vs enemy king
//   * Bishop pawns on same colour (depends on bishop set)
//   * Knight outpost (depends on knight + pawn attacks)
//
// Cache layout: power-of-two open-addressed, key-verified probe, no
// chaining, overwrite-on-collision. 16384 entries × 24 B = 384 KB per
// thread. Thread-local — no concurrency, no atomics.

#pragma once

#include "types.h"

namespace hypersion::Eval {

struct PawnEntry {
    Key       key      = 0;            // pawn_key, 0 = empty slot
    int       mg       = 0;            // cached pure-pawn mg contribution
    int       eg       = 0;            // cached pure-pawn eg contribution
    Bitboard  passed[2] = { 0, 0 };    // passed pawns per colour
};

constexpr int PAWN_HASH_SIZE = 16384;   // 2^14
static_assert((PAWN_HASH_SIZE & (PAWN_HASH_SIZE - 1)) == 0,
              "PAWN_HASH_SIZE must be power of two");

// Thread-local storage — every search worker has its own table.
inline thread_local PawnEntry g_pawn_hash[PAWN_HASH_SIZE] = {};

inline PawnEntry* pawn_hash_probe(Key k) {
    return &g_pawn_hash[k & (PAWN_HASH_SIZE - 1)];
}

}  // namespace hypersion::Eval
