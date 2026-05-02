// Hypersion — transposition table implementation.
// Replacement strategy:
//   * Always overwrite an empty slot.
//   * Otherwise pick the slot whose age + depth is lowest (Stockfish's formula).

#include "tt.h"

#include <cstring>
#include <new>

namespace hypersion {

TranspositionTable TT;

TranspositionTable::~TranspositionTable() { ::operator delete[](table, std::align_val_t(64)); }

void TranspositionTable::resize(std::size_t mbSize) {
    std::size_t bytes = mbSize * 1024 * 1024;
    std::size_t newCount = bytes / sizeof(Cluster);
    if (newCount == clusterCount) { clear(); return; }

    ::operator delete[](table, std::align_val_t(64));
    table = static_cast<Cluster*>(::operator new[](newCount * sizeof(Cluster), std::align_val_t(64)));
    clusterCount = newCount;
    clear();
}

void TranspositionTable::clear() {
    if (table && clusterCount) std::memset(table, 0, clusterCount * sizeof(Cluster));
    generation8 = 0;
}

TTEntry* TranspositionTable::probe(Key key, bool& found) const {
    // Stockfish's mulhi indexing: hi 32 bits of (key * clusterCount).
    Cluster& c = table[(std::uint64_t(std::uint32_t(key)) * std::uint64_t(clusterCount)) >> 32];

    std::uint16_t key16 = std::uint16_t(key);

    // First pass: check every slot for a real key match. Empty slots are skipped here
    // because returning early on an empty slot would miss a hit in a later slot.
    for (int i = 0; i < CLUSTER_ENTRIES; ++i) {
        TTEntry& e = c.entries[i];
        if ((e.genBound8 & 0x3) != 0 && e.key16 == key16) {
            e.genBound8 = std::uint8_t(generation8 | (e.genBound8 & 0x3));
            found = true;
            return &e;
        }
    }

    // No key match. Prefer an empty slot; else replace the worst entry.
    TTEntry* replace = &c.entries[0];
    for (int i = 0; i < CLUSTER_ENTRIES; ++i) {
        TTEntry& e = c.entries[i];
        if ((e.genBound8 & 0x3) == 0) { found = false; return &e; }
        if (i == 0) continue;
        int rep_score = replace->depth8
                       - ((263 + generation8 - replace->genBound8) & 0xFC) * 2;
        int e_score   = e.depth8
                       - ((263 + generation8 - e.genBound8) & 0xFC) * 2;
        if (e_score < rep_score) replace = &e;
    }
    found = false;
    return replace;
}

int TranspositionTable::hashfull() const {
    if (!clusterCount) return 0;
    int cnt = 0;
    std::size_t samples = std::min<std::size_t>(1000, clusterCount);
    for (std::size_t i = 0; i < samples; ++i)
        for (int j = 0; j < CLUSTER_ENTRIES; ++j)
            if ((table[i].entries[j].genBound8 & 0x3) != 0
                && (table[i].entries[j].genBound8 & 0xFC) == generation8)
                ++cnt;
    return cnt * 1000 / (samples * CLUSTER_ENTRIES);
}

Value TranspositionTable::value_to_tt(Value v, int ply) {
    return  v >=  VALUE_MATE_IN_MAX_PLY ? Value(v + ply)
          : v <= -VALUE_MATE_IN_MAX_PLY ? Value(v - ply)
                                        : v;
}
Value TranspositionTable::value_from_tt(Value v, int ply, int /*rule50*/) {
    if (v == VALUE_NONE) return VALUE_NONE;
    if (v >=  VALUE_MATE_IN_MAX_PLY) return Value(v - ply);
    if (v <= -VALUE_MATE_IN_MAX_PLY) return Value(v + ply);
    return v;
}

void TTEntry::save(Key k, Value v, bool /*pv*/, Bound b, Depth d, Move m, Value e, int gen) {
    std::uint16_t k16 = std::uint16_t(k);

    // Preserve any existing TT move on a non-EXACT save (so we don't lose a
    // good move just because we hit the slot at lower depth without a move).
    if (m != Move::none() || k16 != key16) move16 = std::uint16_t(m.raw());
    if (b == BOUND_EXACT
        || k16 != key16
        || d - 4 > depth8 - 4) {
        key16     = k16;
        depth8    = std::uint8_t(d);
        genBound8 = std::uint8_t(gen | b);
        value16   = std::int16_t(v);
        eval16    = std::int16_t(e);
    }
}

}  // namespace hypersion
