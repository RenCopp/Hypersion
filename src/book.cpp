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

// ─── Cross-session opening variety (ported from Kirin V8) ──────────────
// Track the last N first-moves the bot played from the starting position
// across sessions. When probing from startpos, EXCLUDE recently-played
// first moves so the bot doesn't repeat e2e4 game after game.
//
// State file lives next to the executable. Idea + algorithm credit:
// Kirin V8 (C:\Engine\Kirin V8\kirin_engine.py OpeningBook).
namespace {

constexpr int OPENING_HISTORY_MAX = 16;
const char*   OPENING_HISTORY_FILE = "hypersion_recent_openings.txt";

std::vector<std::string>& recent_first_moves() {
    static std::vector<std::string> recent;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(OPENING_HISTORY_FILE);
        std::string line;
        while (std::getline(f, line)) {
            // strip whitespace
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                line.pop_back();
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
                line.erase(line.begin());
            if (!line.empty() && line.size() <= 5)
                recent.push_back(line);
        }
        if (recent.size() > OPENING_HISTORY_MAX)
            recent.erase(recent.begin(),
                         recent.begin() + (recent.size() - OPENING_HISTORY_MAX));
    }
    return recent;
}

void save_recent() {
    auto& recent = recent_first_moves();
    std::ofstream f(OPENING_HISTORY_FILE, std::ios::trunc);
    if (!f) return;
    for (const auto& m : recent) f << m << '\n';
}

void remember_first_move(const std::string& mvUci) {
    auto& recent = recent_first_moves();
    // Move to-front: remove if already present, then append.
    auto it = std::find(recent.begin(), recent.end(), mvUci);
    if (it != recent.end()) recent.erase(it);
    recent.push_back(mvUci);
    if (recent.size() > OPENING_HISTORY_MAX)
        recent.erase(recent.begin(),
                     recent.begin() + (recent.size() - OPENING_HISTORY_MAX));
    save_recent();
}

// Convert our Move to a polyglot-compatible UCI string for the recent-list
// (so we don't need an external move-formatter dependency).
std::string move_to_uci(const Move m) {
    if (m == Move::none()) return "";
    std::string s;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    s += char('a' + int(file_of(from)));
    s += char('1' + int(rank_of(from)));
    s += char('a' + int(file_of(to)));
    s += char('1' + int(rank_of(to)));
    if (m.type_of() == MT_PROMOTION) {
        constexpr char promoChar[] = " pnbrqk";
        s += promoChar[m.promotion_type()];
    }
    return s;
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

    // ─── Cross-session opening variety (Kirin V8 port) ───────────────
    // At startpos, exclude recently-played first moves so the bot doesn't
    // play the same opening every game. Progressive window-shrink lets
    // us drop the constraint when the book has too few candidates.
    //
    // Startpos detection: canonical piece counts + side-to-move +
    // castling rights + rule50. Skipping the polyglot-key comparison
    // because the piece-count check is unambiguous for standard chess
    // (Chess960 starting positions also have 32 pieces but the bot
    // doesn't currently load a Chess960-specific book, so this stays).
    bool isStartpos = (popcount(pos.pieces()) == 32
                   && popcount(pos.pieces(WHITE, PAWN))   == 8
                   && popcount(pos.pieces(BLACK, PAWN))   == 8
                   && popcount(pos.pieces(WHITE, KNIGHT)) == 2
                   && popcount(pos.pieces(BLACK, KNIGHT)) == 2
                   && pos.side_to_move() == WHITE
                   && pos.rule50_count() == 0
                   && pos.can_castle(WHITE_CASTLING)
                   && pos.can_castle(BLACK_CASTLING));

    if (isStartpos) {
        auto& recent = recent_first_moves();
        if (!recent.empty()) {
            // Progressive shrink: try excluding the last 8, then 7, etc.
            std::vector<Cand> filtered;
            int window = std::min(8, int(recent.size()));
            while (window >= 1) {
                std::vector<std::string> recentSet(recent.end() - window,
                                                    recent.end());
                filtered.clear();
                for (const auto& c : cands) {
                    std::string u = move_to_uci(c.m);
                    bool found = false;
                    for (const auto& r : recentSet)
                        if (u == r) { found = true; break; }
                    if (!found) filtered.push_back(c);
                }
                if (!filtered.empty()) break;
                --window;
            }
            // Last-resort: at least exclude the SINGLE most-recent move
            // so we never play it back-to-back.
            if (filtered.empty() && recent.size() >= 1) {
                const std::string& last = recent.back();
                for (const auto& c : cands)
                    if (move_to_uci(c.m) != last) filtered.push_back(c);
            }
            if (!filtered.empty()) cands = filtered;
        }
    }

    double total = 0;
    for (auto& c : cands) total += c.w;
    std::uniform_real_distribution<double> dist(0.0, total);
    double pick = dist(book_prng());
    double acc = 0;
    Move chosen = cands.back().m;
    for (auto& c : cands) {
        acc += c.w;
        if (acc >= pick) { chosen = c.m; break; }
    }

    // Remember first-move at startpos for the cross-session variety filter.
    if (isStartpos) remember_first_move(move_to_uci(chosen));
    return chosen;
}

}  // namespace hypersion::Book
