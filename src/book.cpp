// Hypersion — Polyglot opening book reader.
//
// Polyglot uses a fixed table of 781 random 64-bit numbers to compute its
// position keys.  The full table (PolyKeys) lives in book_keys.inc and is
// included verbatim; that's the standard table from polyglot.org so any
// .bin file from any Polyglot-generator works with us.

#include "book.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

#include "bitboard.h"
#include "movegen.h"
#include "position.h"

namespace hypersion::Book {

namespace {

// Parsed entries are kept in memory for fast probing — books are small (typically
// < 50 MB even for huge ones, and Perfect2023.bin is ~25 MB).
struct Entry { std::uint64_t key; std::uint16_t move; std::uint16_t weight; std::uint32_t learn; };
std::vector<Entry> g_entries;
bool g_open = false;

inline std::uint16_t read_be16(const char* p) {
    return std::uint16_t((std::uint8_t(p[0]) << 8) | std::uint8_t(p[1]));
}
inline std::uint32_t read_be32(const char* p) {
    return (std::uint32_t(std::uint8_t(p[0])) << 24)
         | (std::uint32_t(std::uint8_t(p[1])) << 16)
         | (std::uint32_t(std::uint8_t(p[2])) <<  8)
         |  std::uint32_t(std::uint8_t(p[3]));
}
inline std::uint64_t read_be64(const char* p) {
    return (std::uint64_t(read_be32(p)) << 32) | read_be32(p + 4);
}

// ---------- Canonical Polyglot keys ----------
// Embedded directly via the auto-generated .inc — see Cfish polybook.c (GPL).
#include "polyglot_keys.inc"

constexpr int POLY_OFFSET_PIECE  = 0;     // 12 * 64 = 768 entries
constexpr int POLY_OFFSET_CASTLE = 768;   // WHITE_OO, WHITE_OOO, BLACK_OO, BLACK_OOO
constexpr int POLY_OFFSET_EP     = 772;   // 8 entries (one per file)
constexpr int POLY_OFFSET_TURN   = 780;   // XOR'd if white to move

}  // namespace

bool is_open() { return g_open; }
void close() { g_entries.clear(); g_open = false; }

bool open(const std::string& path) {
    close();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize bytes = f.tellg();
    f.seekg(0, std::ios::beg);
    if (bytes <= 0 || (bytes % 16) != 0) return false;

    std::size_t count = std::size_t(bytes) / 16;
    g_entries.resize(count);
    std::vector<char> buf;
    buf.resize(std::size_t(bytes));
    f.read(buf.data(), bytes);
    if (!f) { close(); return false; }
    for (std::size_t i = 0; i < count; ++i) {
        const char* p = buf.data() + i * 16;
        g_entries[i].key    = read_be64(p);
        g_entries[i].move   = read_be16(p + 8);
        g_entries[i].weight = read_be16(p + 10);
        g_entries[i].learn  = read_be32(p + 12);
    }
    g_open = true;
    std::fprintf(stderr, "info string book: opened %s (%zu entries)\n",
                 path.c_str(), count);
    return true;
}

std::uint64_t polyglot_key(const Position& pos) {
    std::uint64_t key = 0;

    // Polyglot piece order: BP=0, WP=1, BN=2, WN=3, BB=4, WB=5, BR=6, WR=7,
    //                      BQ=8, WQ=9, BK=10, WK=11.
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Piece pc = pos.piece_on(s);
        if (pc == NO_PIECE) continue;
        int polyPiece = 2 * (int(type_of(pc)) - int(PAWN)) + (color_of(pc) == WHITE ? 1 : 0);
        key ^= CanonicalPolyKeys[POLY_OFFSET_PIECE + 64 * polyPiece + int(s)];
    }
    if (pos.can_castle(WHITE_OO))  key ^= CanonicalPolyKeys[POLY_OFFSET_CASTLE + 0];
    if (pos.can_castle(WHITE_OOO)) key ^= CanonicalPolyKeys[POLY_OFFSET_CASTLE + 1];
    if (pos.can_castle(BLACK_OO))  key ^= CanonicalPolyKeys[POLY_OFFSET_CASTLE + 2];
    if (pos.can_castle(BLACK_OOO)) key ^= CanonicalPolyKeys[POLY_OFFSET_CASTLE + 3];

    // Polyglot only XORs the en-passant key if the side to move actually has
    // a pawn that could legally make the en-passant capture.
    if (pos.ep_square() != SQ_NONE) {
        Square ep = pos.ep_square();
        Color  us = pos.side_to_move();
        Bitboard pawns = pos.pieces(us, PAWN);
        Bitboard captSquares = pawn_attacks_bb(~us, ep);   // squares from which our pawn can take
        if (pawns & captSquares)
            key ^= CanonicalPolyKeys[POLY_OFFSET_EP + int(file_of(ep))];
    }

    if (pos.side_to_move() == WHITE)
        key ^= CanonicalPolyKeys[POLY_OFFSET_TURN];
    return key;
}

namespace {

// Decode a Polyglot move (special encoding) into our Move type, validated against
// `pos`'s legal move list.
Move decode_polyglot_move(std::uint16_t pmove, const Position& pos) {
    int toFile   = (pmove >> 0) & 0x7;
    int toRank   = (pmove >> 3) & 0x7;
    int fromFile = (pmove >> 6) & 0x7;
    int fromRank = (pmove >> 9) & 0x7;
    int promo    = (pmove >> 12) & 0x7;   // 0=none 1=N 2=B 3=R 4=Q

    Square from = make_square(File(fromFile), Rank(fromRank));
    Square to   = make_square(File(toFile),   Rank(toRank));

    for (Move m : MoveList<LEGAL>(pos)) {
        if (m.from_sq() != from || m.to_sq() != to) continue;
        if (promo == 0)              { if (m.type_of() != MT_PROMOTION) return m; }
        else if (m.type_of() == MT_PROMOTION) {
            // Polyglot promo enum maps directly: 1=N, 2=B, 3=R, 4=Q.
            if (int(m.promotion_type()) - int(KNIGHT) + 1 == promo) return m;
        }
    }
    // Polyglot encodes castling as e1g1 / e1c1 etc. Our LEGAL list will return them
    // as MT_CASTLING with same from/to, so the loop above will hit them naturally.
    return Move::none();
}

}  // namespace

// PRNG seeded *once per process* with both random_device AND clock so distinct
// invocations don't all converge on the same opening just because the OS RNG
// returned the same word.
namespace {
std::mt19937& book_prng() {
    static std::mt19937 prng(
        std::random_device{}() ^
        std::uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())
    );
    return prng;
}
}  // namespace

Move probe(const Position& pos, bool pickBest) {
    if (!g_open || g_entries.empty()) return Move::none();
    std::uint64_t k = polyglot_key(pos);

    // Binary-search for the lower bound, then iterate equal keys.
    auto it = std::lower_bound(g_entries.begin(), g_entries.end(), k,
                               [](const Entry& e, std::uint64_t key) { return e.key < key; });
    if (it == g_entries.end() || it->key != k) return Move::none();

    auto end = it;
    while (end != g_entries.end() && end->key == k) ++end;

    if (pickBest) {
        Entry* best = nullptr;
        for (auto p = it; p != end; ++p)
            if (!best || p->weight > best->weight) best = &*p;
        return best ? decode_polyglot_move(best->move, pos) : Move::none();
    }

    // Find the highest weight, then keep all moves with weight >= 1/12 of it.
    // The previous /4 threshold + sqrt weighting was too aggressive — first
    // book move varied but follow-ups converged on the same line, so playing
    // the same opponent twice gave near-identical games. Looser threshold
    // surfaces minority lines; uniform weighting then picks among the
    // surviving "playable" moves with no preference. Result: same opening
    // family but real shuffle within the lines the book considers reasonable.
    std::uint16_t bestW = 0;
    for (auto p = it; p != end; ++p) if (p->weight > bestW) bestW = p->weight;
    std::uint16_t threshold = std::max<std::uint16_t>(1, bestW / 12);

    struct Cand { Move m; double w; };
    std::vector<Cand> cands;
    cands.reserve(16);
    for (auto p = it; p != end; ++p) {
        if (p->weight < threshold) continue;
        Move m = decode_polyglot_move(p->move, pos);
        if (m == Move::none()) continue;
        cands.push_back({ m, 1.0 });   // uniform — any surviving move equally likely
    }
    if (cands.empty()) return Move::none();

    double total = 0;
    for (auto& c : cands) total += c.w;
    std::uniform_real_distribution<double> dist(0.0, total);
    double pick = dist(book_prng());
    double acc = 0;
    for (auto& c : cands) {
        acc += c.w;
        if (acc >= pick) return c.m;
    }
    return cands.back().m;
}

}  // namespace hypersion::Book
