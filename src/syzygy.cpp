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
    // NOTE pre-2026-05-12: this returned false when rule50_count() != 0.
    // That was wrong — Fathom's tb_probe_root accepts rule50 as input and
    // returns WDL+suggested-move regardless. The check made the engine skip
    // TB consultation in every K+P / K+R+P / endgame conversion past the
    // first non-zeroing move, falling back to NNUE eval and king-shuffling
    // (cf. user PGN game 2 endgame, drawn by 100-move adjudication despite
    // a winning advantage). Codex audit 2026-05-12.
    //
    // The actual fix uses probe_root_dtz() below for SF18-style per-move
    // ranking. This legacy single-move probe is kept for back-compat in
    // case any external code still calls it; it's no longer the path the
    // search uses.

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

// SF18-style per-move ranking via Fathom's tb_probe_root_dtz. Used at root
// to filter+order rootMoves by (WDL, DTZ): the move that preserves the win
// AND has smallest DTZ floats to the top. Crucially this works at any
// rule50 — Fathom takes rule50 as an input and uses it correctly.
bool probe_root_dtz(const Position& pos, std::vector<RootMoveEntry>& out) {
    out.clear();
    if (!g_loaded) return false;
    if (popcount(pos.pieces()) > int(TB_LARGEST)) return false;
    if (pos.can_castle(WHITE_CASTLING) || pos.can_castle(BLACK_CASTLING)) return false;

    // Workaround for Fathom-library hang on certain KQK / KRK positions
    // where probe_dtz recursion blows up: skip root DTZ probe ONLY for
    // those specific material configs where the regular search trivially
    // finds the win in <1s.
    //
    // Repro: position fen 8/8/8/4k3/8/8/8/Q3K3 w - - 0 1 + Syzygy loaded
    // -> tb_probe_root_dtz never returns. Other KQK configurations
    // (e.g., kings off the same file) probe fine. Likely a Fathom
    // probe_dtz_table edge case for specific king-on-same-file configs.
    //
    // 2026-05-16 NARROWED: the previous version of this skip caught ALL
    // 4-piece-no-pawn positions including KBNK. KBNK is a known-difficult
    // endgame where NNUE+search alone shuffles indefinitely (depth-28
    // NNUE-on test: score cp 236, no mate found, drawn by 50-move rule
    // in actual play). DTZ root ranking is the only practical way to
    // convert KBNK at engine TC. Restrict the hang-workaround to KQK
    // and KRK specifically; allow KBNK / KBBK / KNNK / KQBK / KQNK /
    // KRBK / KRNK through to the probe.
    int totalPieces = popcount(pos.pieces());
    if (totalPieces == 3 && pos.pieces(PAWN) == 0) {
        // 3-piece pawnless: KvK / KB / KN / KR / KQ vs lone K — trivial,
        // drawn, or covered by simple search. KQK with kings on the same
        // file is the original documented Fathom hang shape; skipping
        // all 3-piece pawnless avoids it.
        // KPK (3-piece WITH pawn) is NOT skipped — DTZ guidance is
        // essential for promotion + mate path; without it the engine
        // can shuffle past the 50-move rule before promoting.
        return false;
    }
    if (totalPieces == 4 && pos.pieces(PAWN) == 0) {
        // 4-piece, no pawn. Skip ONLY for known-hang KQK and KRK shapes
        // (i.e., a queen or rook is the only minor/major + 2 kings).
        bool hasQ = pos.pieces(QUEEN)  != 0;
        bool hasR = pos.pieces(ROOK)   != 0;
        bool hasB = pos.pieces(BISHOP) != 0;
        bool hasN = pos.pieces(KNIGHT) != 0;
        bool sf_hang_shape = (hasQ || hasR) && !hasB && !hasN;
        if (sf_hang_shape) return false;
        // KBNK / KBBK / KNNK fall through to the probe — they need DTZ
        // ranking to drive the lone king to the correct corner inside
        // the 50-move budget.
    }

    TBQuery q = build_query(pos);
    TbRootMoves results;
    results.size = 0;
    int rc = tb_probe_root_dtz(q.white, q.black, q.kings, q.queens, q.rooks,
                               q.bishops, q.knights, q.pawns,
                               q.rule50, q.castling, q.ep, q.turn,
                               /*hasRepeated=*/false,
                               /*useRule50=*/g_50_move_rule,
                               &results);
    if (rc == 0 || results.size == 0) return false;

    out.reserve(results.size);
    for (unsigned i = 0; i < results.size; ++i) {
        const TbRootMove& tbm = results.moves[i];
        // Fathom's TbMove uses MOVE_FROM/MOVE_TO/MOVE_PROMOTES — see
        // tbprobe.h:295-302. Promotion code: 1=Q,2=R,3=B,4=N.
        Square fromSq = Square(TB_MOVE_FROM(tbm.move));
        Square toSq   = Square(TB_MOVE_TO  (tbm.move));
        unsigned promo = TB_MOVE_PROMOTES (tbm.move);
        Move m;
        if (promo) {
            constexpr PieceType ptMap[5] = { NO_PIECE_TYPE, QUEEN, ROOK, BISHOP, KNIGHT };
            m = Move::make(fromSq, toSq, MT_PROMOTION, ptMap[promo]);
        } else {
            m = Move(fromSq, toSq);
        }

        // tbScore: Fathom uses a centipawn-like scale (mate-adjacent for
        // winning, 0 for draw, negative for loss). tbRank: 0..1000 for
        // wins, with 1000 = optimal. We map tbScore -> Value via a clamp
        // that keeps it in the comfortable TB-score band so it doesn't
        // clash with mate scores during display.
        Value v;
        if      (tbm.tbScore >  900) v = Value(VALUE_TB_WIN - 100);
        else if (tbm.tbScore < -900) v = Value(-VALUE_TB_WIN + 100);
        else                         v = Value(tbm.tbScore);

        // Use Fathom's rank directly; this scale puts winning-optimal at
        // the top and is comparable across moves at the same root.
        out.push_back({ m, tbm.tbRank, v, /*wdl=*/0 });
    }
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
