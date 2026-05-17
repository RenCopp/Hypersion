// Hypersion — Position class.
// Holds the board state and supports do_move / undo_move with a rollback stack
// of StateInfo nodes. Move generation, search and evaluation all read from the
// Position; only do_move / undo_move and FEN parsing mutate it.
//
// Standard rules only for now (no Chess960 / FRC). Rook squares are still
// tracked per side+side-of-king so the same code can be extended later.

#ifndef HYPERSION_POSITION_H
#define HYPERSION_POSITION_H

#include <array>
#include <iosfwd>
#include <string>

#include "bitboard.h"
#include "types.h"

namespace hypersion {

// ---------------------------------------------------------------------------
// DirtyPiece — records a single piece transition produced by do_move so the
// NNUE accumulator can be updated incrementally without re-walking the board.
// `from = SQ_NONE` means the piece was added (e.g. promotion result).
// `to   = SQ_NONE` means the piece was removed (capture / pawn promoted away).
struct DirtyPiece {
    Piece  pc;
    Square from;
    Square to;
};

// NNUE accumulator state, one slot per StateInfo. Populated lazily on demand
// by hypersion::NNUE::make_valid(); evaluate() only walks the search-stack
// chain and applies DirtyPiece deltas. Sized for the SF18 SFNNv10
// architecture: big = 1024-d FT, small = 128-d FT.
//
// 64-byte alignment matches one cache line — eliminates the rare case where
// a single accumulator straddles a line boundary and forces two L1d loads.
// AVX-VNNI operates on 32-byte vectors so this only matters cosmetically for
// the inner loops, but the cache-line alignment is a free win for streaming.
struct NNUEAccState {
    alignas(64) std::int16_t big_acc  [2][1024];
    alignas(64) std::int16_t small_acc[2][128];
    std::int32_t big_psqt  [2][8];
    std::int32_t small_psqt[2][8];
    bool valid_big  [2] = { false, false };
    bool valid_small[2] = { false, false };
};

// StateInfo — one entry per ply on the rollback stack.
// Fields above the comment are needed for undo_move; below are derived caches
// that the next ply's set_check_info() recomputes (kept here so search can
// look at the parent ply without recomputing).
// ---------------------------------------------------------------------------
struct StateInfo {
    Key      pawnKey;
    Key      materialKey;
    Key      nonPawnKey[COLOR_NB];   // hash of all non-pawn pieces, per color
    Key      minorKey;               // for correction history (M6+)

    Value    nonPawnMaterial[COLOR_NB];
    int      castlingRights;
    int      rule50;
    int      pliesFromNull;
    Square   epSquare;

    // -------- Below: copied/recomputed each do_move ------------------------
    Key      key;
    Bitboard checkersBB;
    Piece    capturedPiece;
    int      repetition;             // # of times this key seen in stack (0,1,2)

    // Pin / check geometry — refreshed by Position::set_check_info() after each move.
    Bitboard blockersForKing[COLOR_NB];
    Bitboard pinners       [COLOR_NB];
    Bitboard checkSquares  [PIECE_TYPE_NB];

    StateInfo* previous;

    // ---- NNUE incremental-update bookkeeping ------------------------------
    // dirtyPiece[0..dirtyCount) records what do_move changed in piece state
    // since `previous`. NNUE walks back through the chain applying these.
    // Worst case: castling = 2 dirty pieces (king + rook), capture-promotion
    // = 3 (captured + pawn-removed + promo-added). dirtyCount = 0 at root.
    DirtyPiece   dirtyPiece[3];
    int          dirtyCount;

    // Lazy NNUE accumulator. Big or small slot is filled on the first eval
    // call that needs it; cleared (valid flags reset) on each do_move.
    NNUEAccState nnue;
};

// Move list / undo stack capacity — enough for any realistic game length.
constexpr int MAX_GAME_PLIES = 2048;

// ---------------------------------------------------------------------------
// Position class
// ---------------------------------------------------------------------------
class Position {
public:
    static void init();

    Position() = default;
    Position(const Position&)            = delete;
    Position& operator=(const Position&) = delete;

    // FEN I/O ---------------------------------------------------------------
    Position& set(const std::string& fen, StateInfo* si);
    std::string fen() const;

    // Squares / pieces ------------------------------------------------------
    Piece    piece_on(Square s)             const { return board[s]; }
    bool     empty(Square s)                const { return board[s] == NO_PIECE; }
    Square   ep_square()                    const { return st->epSquare; }

    Bitboard pieces()                       const { return byTypeBB[ALL_PIECES]; }
    Bitboard pieces(PieceType pt)           const { return byTypeBB[pt]; }
    Bitboard pieces(PieceType p1, PieceType p2) const { return byTypeBB[p1] | byTypeBB[p2]; }
    Bitboard pieces(Color c)                const { return byColorBB[c]; }
    Bitboard pieces(Color c, PieceType pt)  const { return byColorBB[c] & byTypeBB[pt]; }
    Bitboard pieces(Color c, PieceType p1, PieceType p2) const { return byColorBB[c] & (byTypeBB[p1] | byTypeBB[p2]); }

    template<PieceType Pt>
    Square square(Color c) const { return lsb(pieces(c, Pt)); }

    int      count(Color c, PieceType pt)   const { return popcount(pieces(c, pt)); }
    int      count(PieceType pt)            const { return popcount(pieces(pt)); }

    // State queries ---------------------------------------------------------
    Color    side_to_move()                 const { return sideToMove; }
    int      game_ply()                     const { return gamePly; }
    int      rule50_count()                 const { return st->rule50; }
    int      castling_rights(Color c)       const { return st->castlingRights & (c == WHITE ? WHITE_CASTLING : BLACK_CASTLING); }
    bool     can_castle(CastlingRights cr)  const { return st->castlingRights & cr; }
    // The actual square of the rook that participates in castling for `cr`.
    // In standard chess this is FILE_H (for OO) or FILE_A (for OOO) on the
    // home rank; in Chess960 it can be any file on the home rank.
    Square   castling_rook_square(CastlingRights cr) const { return castlingRookSquare[cr]; }
    // 2026-05-17 audit uci [25] Chess960: detect whether any castling rook
    // sits on a non-standard file. Caller (fen() output, UCI move emit)
    // uses this to decide between standard `KQkq` and Shredder/X-FEN file
    // notation, and between king-to-G/C destination vs king-takes-rook.
    bool     is_chess960() const {
        for (CastlingRights cr : { WHITE_OO, WHITE_OOO, BLACK_OO, BLACK_OOO })
            if (st->castlingRights & cr) {
                Square rs = castlingRookSquare[cr];
                bool kingSide = (cr == WHITE_OO || cr == BLACK_OO);
                if (kingSide  && file_of(rs) != FILE_H) return true;
                if (!kingSide && file_of(rs) != FILE_A) return true;
            }
        return false;
    }
    StateInfo* state()                      const { return st; }   // for NNUE incremental updates
    Bitboard checkers()                     const { return st->checkersBB; }
    Bitboard blockers_for_king(Color c)     const { return st->blockersForKing[c]; }
    Bitboard pinners(Color c)               const { return st->pinners[c]; }
    Bitboard check_squares(PieceType pt)    const { return st->checkSquares[pt]; }
    Key      key()                          const { return st->key; }
    Key      pawn_key()                     const { return st->pawnKey; }
    Key      material_key()                 const { return st->materialKey; }
    Key      minor_key()                    const { return st->minorKey; }
    Key      non_pawn_key(Color c)          const { return st->nonPawnKey[c]; }
    Value    non_pawn_material(Color c)     const { return st->nonPawnMaterial[c]; }
    Value    non_pawn_material()            const { return st->nonPawnMaterial[WHITE] + st->nonPawnMaterial[BLACK]; }
    Piece    captured_piece()               const { return st->capturedPiece; }
    int      repetition()                   const { return st->repetition; }
    bool     is_draw(int ply) const;
    // 2026-05-17 audit #4: SF18 position.cpp:1432 — returns true if a move
    // exists in this position that draws by repetition (cuckoo table lookup).
    // Lets search return an early VALUE_DRAW alpha-bump when alpha < 0,
    // catching forced-repetition draws that would otherwise be missed.
    bool     upcoming_repetition(int ply) const;

    // Attack queries --------------------------------------------------------
    Bitboard attackers_to(Square s)                   const { return attackers_to(s, pieces()); }
    Bitboard attackers_to(Square s, Bitboard occupied) const;

    // Move properties -------------------------------------------------------
    bool     legal(Move m)         const;
    bool     pseudo_legal(Move m)  const;
    bool     capture(Move m)       const;
    bool     capture_or_promotion(Move m) const;
    bool     gives_check(Move m)   const;
    Piece    moved_piece(Move m)   const { return board[m.from_sq()]; }

    // Static Exchange Evaluation — Stockfish-style swap-off, returns true iff
    // the SEE value of `m` is at least `threshold`. Implementation in
    // position.cpp (see_ge).
    bool     see_ge(Move m, Value threshold = VALUE_ZERO) const;

    // Mutation --------------------------------------------------------------
    void do_move  (Move m, StateInfo& newSt);
    void do_move  (Move m, StateInfo& newSt, bool givesCheck);
    void undo_move(Move m);
    void do_null_move  (StateInfo& newSt);
    void undo_null_move();

    // Recompute st->repetition based on the current StateInfo->previous chain.
    // Use after the chain is reattached externally (e.g. Worker::prepare
    // copying history from srcPos). set() runs this once at FEN parse but
    // can't see history that exists in StateInfos linked AFTER set() returns.
    void recompute_repetition();

    // Debug -----------------------------------------------------------------
    bool        pos_is_ok() const;
    std::string pretty()    const;

private:
    void put_piece (Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);
    void set_castling_right(Color c, Square rfrom);
    void set_check_info();
    void set_state();

    // Slider blockers helper used both by set_check_info() and by
    // legal()/gives_check() probing of discovered checks.
    Bitboard slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const;

    // Board representation
    Piece     board[SQUARE_NB] = {};
    Bitboard  byTypeBB [PIECE_TYPE_NB] = {};
    Bitboard  byColorBB[COLOR_NB]      = {};
    int       pieceCount[PIECE_NB]     = {};

    // Castling — castlingRightsMask[s] tells which rights to clear when a piece
    // moves from (or onto) square s.  castlingRookSquare[cr] = origin rook sq.
    int       castlingRightsMask[SQUARE_NB] = {};
    Square    castlingRookSquare[CASTLING_RIGHT_NB] = {};
    Bitboard  castlingPath[CASTLING_RIGHT_NB] = {};

    Color     sideToMove = WHITE;
    int       gamePly    = 0;
    StateInfo* st        = nullptr;
};

std::ostream& operator<<(std::ostream& os, const Position& pos);

}  // namespace hypersion

#endif  // HYPERSION_POSITION_H
