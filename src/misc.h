// Hypersion — misc utilities: logging, timing, PRNG.

#ifndef HYPERSION_MISC_H
#define HYPERSION_MISC_H

#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <sstream>

namespace hypersion {

using TimePoint = std::chrono::milliseconds::rep;

inline TimePoint now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Thread-safe stdout: avoids interleaved UCI output across search threads.
// Usage:  sync_cout << "info depth " << d << sync_endl;
struct SyncCout { enum Sync { IO_LOCK, IO_UNLOCK }; };
std::ostream& operator<<(std::ostream& os, SyncCout::Sync s);
#define sync_cout  (std::cout << ::hypersion::SyncCout::IO_LOCK)
#define sync_endl  std::endl << ::hypersion::SyncCout::IO_UNLOCK

// Xorshift64* PRNG — tiny, fast, good enough for Zobrist init, magic search,
// eval randomization. NOT cryptographic.
class PRNG {
    std::uint64_t s;
public:
    explicit PRNG(std::uint64_t seed) : s(seed) { /* seed must be nonzero */ }
    std::uint64_t rand() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    // Sparse random: few bits set — useful when searching magic numbers.
    std::uint64_t sparse_rand() { return rand() & rand() & rand(); }
};

// Engine metadata — bumped each release.
// Release policy: only bump version when an A/B match shows >= +200 ELO over
// the previous release. Until then we run as "<current major>-dev".
constexpr const char* ENGINE_NAME    = "Hypersion";
constexpr const char* ENGINE_VERSION = "1-dev";
constexpr const char* ENGINE_AUTHOR  = "RenCopp";

std::string engine_id();

}  // namespace hypersion

#endif  // HYPERSION_MISC_H
