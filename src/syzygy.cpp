// Hypersion — Syzygy tablebase wrapper around Fathom (tbprobe).

#include "syzygy.h"

#include <cstdio>

#include "bitboard.h"
#include "position.h"

extern "C" {
#include "fathom/tbprobe.h"
}

namespace hypersion::Syzygy {

namespace {
bool g_loaded = false;

// Tunable knobs (Stockfish-compatible UCI options).
int  g_probe_depth   = 1;     // SyzygyProbeDepth — search depth threshold
int  g_probe_limit   = 7;     // SyzygyProbeLimit — max pieces to probe
bool g_50_move_rule  = true;  // Syzygy50MoveRule — honour rule50 in lookups

// Convert our Position into the eight bitboard arguments Fathom expects.
struct TBQuery {
    std::uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned      rule50, castling, ep;
    bool          turn;
};

TBQuery build_query(const Position& pos) {
    TBQuery q{};
    q.white   = pos.pieces(WHITE);
    q.black   = pos.pieces(BLACK);
    q.kings   = pos.pieces(KING);
    q.queens  = pos.pieces(QUEEN);
    q.rooks   = pos.pieces(ROOK);
    q.bishops = pos.pieces(BISHOP);
    q.knights = pos.pieces(KNIGHT);
    q.pawns   = pos.pieces(PAWN);
    q.rule50  = unsigned(pos.rule50_count());
    q.castling = 0;
    if (pos.can_castle(WHITE_OO))  q.castling |= TB_CASTLING_K;
    if (pos.can_castle(WHITE_OOO)) q.castling |= TB_CASTLING_Q;
    if (pos.can_castle(BLACK_OO))  q.castling |= TB_CASTLING_k;
    if (pos.can_castle(BLACK_OOO)) q.castling |= TB_CASTLING_q;
    q.ep   = pos.ep_square() == SQ_NONE ? 0u : unsigned(pos.ep_square());
    q.turn = pos.side_to_move() == WHITE;
    return q;
}

Move decode_tb_move(unsigned res, const Position& pos) {
    if (res == TB_RESULT_FAILED || res == TB_RESULT_CHECKMATE || res == TB_RESULT_STALEMATE)
        return Move::none();
    int from = TB_GET_FROM(res);
    int to   = TB_GET_TO(res);
    int promo = TB_GET_PROMOTES(res);
    Square fromSq = Square(from), toSq = Square(to);
    if (promo) {
        // Fathom: 1=Q,2=R,3=B,4=N — map to our PieceType enum.
        constexpr PieceType ptMap[5] = { NO_PIECE_TYPE, QUEEN, ROOK, BISHOP, KNIGHT };
        return Move::make(fromSq, toSq, MT_PROMOTION, ptMap[promo]);
    }
    if (TB_GET_EP(res)) return Move::make(fromSq, toSq, MT_EN_PASSANT);
    return Move(fromSq, toSq);
}
}  // namespace

bool init(const std::string& path) {
    if (path.empty() || path == "<empty>") {
        if (g_loaded) { tb_free(); g_loaded = false; }
        return false;
    }
    bool ok = tb_init(path.c_str());
    g_loaded = ok && TB_LARGEST > 0;
    std::fprintf(stderr,
        "info string syzygy: %s (largest=%u, path=%s)\n",
        g_loaded ? "loaded" : "no tables found", TB_LARGEST, path.c_str());
    return g_loaded;
}

bool is_loaded() { return g_loaded; }
int  largest()   { return int(TB_LARGEST); }

bool probe_root(const Position& pos, RootProbe& out) {
    if (!g_loaded) return false;
    if (popcount(pos.pieces()) > int(TB_LARGEST)) return false;
    if (pos.can_castle(WHITE_CASTLING) || pos.can_castle(BLACK_CASTLING)) return false;
    if (pos.rule50_count() != 0) return false;     // DTZ probes need rule50 = 0

    TBQuery q = build_query(pos);
    unsigned res = tb_probe_root(q.white, q.black, q.kings, q.queens, q.rooks,
                                 q.bishops, q.knights, q.pawns,
                                 q.rule50, q.castling, q.ep, q.turn, nullptr);
    if (res == TB_RESULT_FAILED) return false;

    int wdl = TB_GET_WDL(res);
    out.bestMove = decode_tb_move(res, pos);
    if      (wdl == TB_WIN)         out.score = Value(VALUE_TB_WIN - 100);
    else if (wdl == TB_CURSED_WIN)  out.score = Value(50);
    else if (wdl == TB_BLESSED_LOSS) out.score = Value(-50);
    else if (wdl == TB_LOSS)        out.score = Value(-VALUE_TB_WIN + 100);
    else                            out.score = VALUE_DRAW;
    out.wdl = wdl - 2;     // shift to -2..+2
    return true;
}

Value probe_wdl(const Position& pos) {
    if (!g_loaded) return VALUE_NONE;
    int pc = popcount(pos.pieces());
    if (pc > int(TB_LARGEST)) return VALUE_NONE;
    if (pc > g_probe_limit) return VALUE_NONE;     // SyzygyProbeLimit gate
    if (pos.can_castle(WHITE_CASTLING) || pos.can_castle(BLACK_CASTLING)) return VALUE_NONE;

    TBQuery q = build_query(pos);
    // Syzygy50MoveRule: Fathom honours rule50 only when we pass a non-zero
    // value. Zero tells Fathom to ignore the 50-move counter (cursed-win
    // positions remain wins, not draws).
    unsigned tb_rule50 = g_50_move_rule ? q.rule50 : 0;
    unsigned r = tb_probe_wdl(q.white, q.black, q.kings, q.queens, q.rooks,
                              q.bishops, q.knights, q.pawns,
                              tb_rule50, q.castling, q.ep, q.turn);
    if (r == TB_RESULT_FAILED) return VALUE_NONE;
    if (r == TB_WIN)          return Value(VALUE_TB_WIN - 100);
    if (r == TB_CURSED_WIN)   return Value(50);
    if (r == TB_DRAW)         return VALUE_DRAW;
    if (r == TB_BLESSED_LOSS) return Value(-50);
    return Value(-VALUE_TB_WIN + 100);
}

// ---- Tunable knob accessors (UCI-driven) ------------------------------------
void set_probe_depth(int d)   { g_probe_depth = d; }
int  probe_depth()             { return g_probe_depth; }
void set_probe_limit(int n)   { g_probe_limit = n; }
int  probe_limit()             { return g_probe_limit; }
void set_50_move_rule(bool b) { g_50_move_rule = b; }
bool fifty_move_rule()         { return g_50_move_rule; }

}  // namespace hypersion::Syzygy
