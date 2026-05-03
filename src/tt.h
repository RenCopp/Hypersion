// Hypersion — transposition table.
//
// Stockfish-style 3-entry clusters laid out in 32-byte cache lines.  Each
// entry stores 16 bits of the position key (lookup key check), the best move
// from that position, search depth, value, static eval, bound flag and a 6-bit
// generation counter (used for aging across moves so older entries get
// replaced first).
//
// Mate values are adjusted by ply on store (depth-from-root → distance-to-mate)
// and back on probe (distance-to-mate → depth-from-root).

#ifndef HYPERSION_TT_H
#define HYPERSION_TT_H

#include <cstdint>
#include <vector>

#include "types.h"

namespace hypersion {

// 10-byte entry; three of these plus 2 padding bytes fit in 32 B.
//
// depth8 layout: bit 7 = ttPv flag, bits 6..0 = search depth (max 127, ample).
// genBound8 layout: bits 7..2 = generation (6 bits = wraps every 64 searches),
//                   bits 1..0 = bound (2 bits).
struct TTEntry {
    std::uint16_t key16;
    std::uint8_t  depth8;      // bit 7 = ttPv, bits 6..0 = depth
    std::uint8_t  genBound8;
    std::uint16_t move16;
    std::int16_t  value16;
    std::int16_t  eval16;

    Move  move()      const { return Move(move16); }
    int   depth()     const { return int(depth8 & 0x7F); }
    bool  is_pv()     const { return (depth8 & 0x80) != 0; }
    Bound bound()     const { return Bound(genBound8 & 0x3); }
    int   generation()const { return genBound8 >> 2; }
    Value value()     const { return Value(value16); }
    Value eval()      const { return Value(eval16); }

    void save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value e, int gen);
};

constexpr int CLUSTER_ENTRIES = 3;
struct alignas(32) Cluster {
    TTEntry entries[CLUSTER_ENTRIES];
    char    padding[32 - sizeof(TTEntry) * CLUSTER_ENTRIES];
};
static_assert(sizeof(Cluster) == 32, "Cluster must be exactly 32 bytes");

class TranspositionTable {
public:
    ~TranspositionTable();

    void resize(std::size_t mbSize);
    void clear();
    void new_search() { generation8 += 4; }   // bottom 2 bits hold the bound
    int  hashfull() const;                    // permille of "active" (current-gen) entries
    int  generation() const { return generation8; }

    // Probe by key. Returns:
    //   tte    : pointer to the matching entry slot (or replacement candidate if !found)
    //   found  : whether key matches and depth is valid
    //   key    : the full 64-bit key, for save() to write the high 16 bits
    TTEntry* probe(Key key, bool& found) const;

    // Hint to the CPU that the cluster for `key` will be touched soon.
    // Issued immediately after do_move so the load is already in flight by
    // the time the recursive search calls probe().
    void prefetch(Key key) const {
        if (!table) return;
        auto* p = &table[(std::uint64_t(std::uint32_t(key)) * std::uint64_t(clusterCount)) >> 32];
        __builtin_prefetch(p);
    }

    // Convert a mate score from "distance-to-root" (search-relative) to
    // "distance-from-root-of-this-mate" before storing, and the inverse after probing.
    static Value value_to_tt  (Value v, int ply);
    static Value value_from_tt(Value v, int ply, int rule50);

private:
    Cluster* table = nullptr;
    std::size_t clusterCount = 0;
    std::uint8_t generation8 = 0;
};

extern TranspositionTable TT;

}  // namespace hypersion

#endif  // HYPERSION_TT_H
