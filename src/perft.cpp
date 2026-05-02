// Hypersion — perft test driver.

#include "perft.h"

#include <iomanip>
#include <iostream>
#include <string>

#include "misc.h"
#include "movegen.h"

namespace hypersion {

std::uint64_t perft(Position& pos, int depth) {
    if (depth <= 0) return 1;

    MoveList<LEGAL> ml(pos);
    if (depth == 1) return std::uint64_t(ml.size());

    std::uint64_t nodes = 0;
    StateInfo st;
    for (Move m : ml) {
        pos.do_move(m, st);
        nodes += perft(pos, depth - 1);
        pos.undo_move(m);
    }
    return nodes;
}

namespace {
std::string move_to_uci(Move m) {
    if (m == Move::none()) return "(none)";
    if (m == Move::null()) return "0000";
    Square from = m.from_sq(), to = m.to_sq();
    std::string s;
    s += char('a' + file_of(from));
    s += char('1' + rank_of(from));
    s += char('a' + file_of(to));
    s += char('1' + rank_of(to));
    if (m.type_of() == MT_PROMOTION) {
        constexpr char promoChar[] = " pnbrqk";
        s += promoChar[m.promotion_type()];
    }
    return s;
}
}  // namespace

void perft_divide(Position& pos, int depth) {
    std::uint64_t total = 0;
    StateInfo st;
    for (Move m : MoveList<LEGAL>(pos)) {
        pos.do_move(m, st);
        std::uint64_t cnt = depth > 1 ? perft(pos, depth - 1) : 1;
        pos.undo_move(m);
        total += cnt;
        std::cout << move_to_uci(m) << ": " << cnt << '\n';
    }
    std::cout << "\nTotal: " << total << " moves\n";
}

void perft_run_suite() {
    // Standard Kiwipete + others used across the chessprogramming community.
    struct Case { const char* fen; int depth; std::uint64_t expected; const char* name; };
    static const Case cases[] = {
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 6, 119060324ULL, "Startpos" },
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 5, 193690690ULL, "Kiwipete" },
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 7, 178633661ULL, "Position 3" },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 6, 706045033ULL, "Position 4" },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 5, 89941194ULL, "Position 5" },
        { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 5, 164075551ULL, "Position 6" },
        { "8/8/8/8/8/8/6k1/4K2R w K - 0 1", 6, 185867ULL, "K vs k+R endgame" },
    };

    Position pos;
    StateInfo si;
    int passed = 0, total = 0;
    for (const auto& c : cases) {
        pos.set(c.fen, &si);
        TimePoint t0 = now();
        std::uint64_t got = perft(pos, c.depth);
        TimePoint dt = now() - t0;

        bool ok = (got == c.expected);
        std::cout << (ok ? "[OK]   " : "[FAIL] ")
                  << c.name << " depth " << c.depth
                  << " : got " << got
                  << ", expected " << c.expected
                  << "  (" << dt << " ms";
        if (dt > 0) std::cout << ", " << got / std::uint64_t(dt) << " kn/s";
        std::cout << ")\n";
        if (ok) ++passed;
        ++total;
    }
    std::cout << "\nperft suite: " << passed << "/" << total << " passed\n";
}

}  // namespace hypersion
