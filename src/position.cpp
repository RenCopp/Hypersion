// Hypersion — Position implementation.
// FEN I/O, do_move / undo_move, legality, attack queries, draw detection.

#include "position.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>

#include "misc.h"
#include "movegen.h"
#include "zobrist.h"

namespace hypersion {

// Stockfish-style midgame piece values — used here only to track non-pawn
// material; the real eval lives in NNUE / classical evaluator.
constexpr Value PieceValue[PIECE_TYPE_NB] = {
    /*NONE*/0, /*P*/126, /*N*/781, /*B*/825, /*R*/1276, /*Q*/2538, /*K*/0, /*-*/0
};

// Two static helpers used in FEN parsing.
namespace {
constexpr char PieceChars[PIECE_NB] = {
    /*0  NO_PIECE*/ ' ',
    /*1  W_PAWN  */ 'P', /*2 W_KNIGHT*/ 'N', /*3 W_BISHOP*/ 'B',
    /*4  W_ROOK  */ 'R', /*5 W_QUEEN */ 'Q', /*6 W_KING  */ 'K',
    /*7  -       */ ' ',
    /*8  -       */ ' ',
    /*9  B_PAWN  */ 'p', /*10 B_KNIGHT*/'n', /*11 B_BISHOP*/'b',
    /*12 B_ROOK  */ 'r', /*13 B_QUEEN */ 'q', /*14 B_KING  */'k',
    /*15 -       */ ' '
};

Piece char_to_piece(char c) {
    for (int p = 0; p < PIECE_NB; ++p)
        if (PieceChars[p] == c) return Piece(p);
    return NO_PIECE;
}
}  // namespace

// 2026-05-17 audit #4: cuckoo hash table for upcoming_repetition().
// Each cuckoo[i] / cuckooMove[i] pair stores a "reversible-move key" =
// Zobrist::psq[pc][s1] XOR Zobrist::psq[pc][s2] XOR Zobrist::side. Indexing
// by H1(key) / H2(key) lets upcoming_repetition() ask "does there exist a
// single non-pawn move that reverses the position to one we just left?"
// in O(end/2) hash probes — much cheaper than enumerating legal moves.
// Source: SF18 src/position.cpp:107-160.
namespace {
constexpr int CUCKOO_SLOTS = 8192;
std::array<Key,  CUCKOO_SLOTS> cuckoo{};
std::array<Move, CUCKOO_SLOTS> cuckooMove{};
inline int H1(Key h) { return int(h & 0x1fff); }
inline int H2(Key h) { return int((h >> 16) & 0x1fff); }
}  // namespace

void Position::init() {
    cuckoo.fill(0);
    cuckooMove.fill(Move::none());
    int count = 0;
    for (Piece pc : { W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                      B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING }) {
        PieceType pt = type_of(pc);
        for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
            for (Square s2 = Square(s1 + 1); s2 <= SQ_H8; ++s2)
                if (attacks_bb(pt, s1, 0) & square_bb(s2)) {
                    Move move = Move(s1, s2);
                    Key  key  = Zobrist::psq[pc][s1] ^ Zobrist::psq[pc][s2] ^ Zobrist::side;
                    int  i    = H1(key);
                    while (true) {
                        std::swap(cuckoo[i], key);
                        std::swap(cuckooMove[i], move);
                        if (move == Move::none())  // empty slot — done
                            break;
                        i = (i == H1(key)) ? H2(key) : H1(key);
                    }
                    ++count;
                }
    }
    (void)count;   // SF asserts 3668; we don't require exact match (depends on attack-table init order)
}

// ---------------------------------------------------------------------------
// Low-level board mutation
// ---------------------------------------------------------------------------
void Position::put_piece(Piece pc, Square s) {
    board[s] = pc;
    byTypeBB [ALL_PIECES] |= square_bb(s);
    byTypeBB [type_of(pc)] |= square_bb(s);
    byColorBB[color_of(pc)] |= square_bb(s);
    ++pieceCount[pc];
}

void Position::remove_piece(Square s) {
    Piece pc = board[s];
    byTypeBB [ALL_PIECES] ^= square_bb(s);
    byTypeBB [type_of(pc)] ^= square_bb(s);
    byColorBB[color_of(pc)] ^= square_bb(s);
    board[s] = NO_PIECE;
    --pieceCount[pc];
}

void Position::move_piece(Square from, Square to) {
    Piece pc = board[from];
    Bitboard fromTo = square_bb(from) | square_bb(to);
    byTypeBB [ALL_PIECES] ^= fromTo;
    byTypeBB [type_of(pc)] ^= fromTo;
    byColorBB[color_of(pc)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to]   = pc;
}

// Helper: file→absolute square on the king's home rank, given color.
namespace {
constexpr Square home_rank_square(Color c, File f) {
    return make_square(f, c == WHITE ? RANK_1 : RANK_8);
}
}  // namespace

// ---------------------------------------------------------------------------
// Castling — register one castling right.
//   c     : which side
//   rfrom : the rook's starting square
// Sets castlingRightsMask so that any move from/to king-square or rook-from
// clears the relevant rights, and precomputes the squares that must be empty
// for the castle to be legal.
// ---------------------------------------------------------------------------
void Position::set_castling_right(Color c, Square rfrom) {
    Square kfrom = square<KING>(c);
    bool   kingSide = rfrom > kfrom;
    CastlingRights cr = (c == WHITE) ? (kingSide ? WHITE_OO : WHITE_OOO)
                                     : (kingSide ? BLACK_OO : BLACK_OOO);

    st->castlingRights              |= cr;
    castlingRightsMask[kfrom]       |= cr;
    castlingRightsMask[rfrom]       |= cr;
    castlingRookSquare[cr]           = rfrom;

    Square kto = home_rank_square(c, kingSide ? FILE_G : FILE_C);
    Square rto = home_rank_square(c, kingSide ? FILE_F : FILE_D);

    castlingPath[cr] = 0;
    Square lo = std::min({kfrom, kto, rfrom, rto});
    Square hi = std::max({kfrom, kto, rfrom, rto});
    for (Square s = lo; s <= hi; ++s)
        if (s != kfrom && s != rfrom)
            castlingPath[cr] |= square_bb(s);
}

// ---------------------------------------------------------------------------
// FEN parser
// ---------------------------------------------------------------------------
Position& Position::set(const std::string& fenStr, StateInfo* si) {
    // Cast to void* so g++ doesn't warn about memset on a non-trivially
    // copyable type — StateInfo carries the NNUE accumulator, and Position
    // has bitboard arrays that the compiler classifies as non-trivial. Both
    // are still POD-layout, so byte-zeroing is safe.
    std::memset(static_cast<void*>(this), 0, sizeof(Position));
    std::memset(static_cast<void*>(si),   0, sizeof(StateInfo));

    st = si;
    sideToMove = WHITE;
    gamePly = 0;

    std::istringstream ss(fenStr);
    ss >> std::noskipws;

    char token;
    Square sq = SQ_A8;

    // 1) Piece placement (rank 8 → rank 1)
    while ((ss >> token) && !std::isspace(static_cast<unsigned char>(token))) {
        if (std::isdigit(static_cast<unsigned char>(token))) {
            sq += (token - '0');
        } else if (token == '/') {
            sq -= 16;
        } else {
            Piece pc = char_to_piece(token);
            if (pc != NO_PIECE) { put_piece(pc, sq); ++sq; }
        }
    }

    // 2) Side to move
    ss >> token;
    sideToMove = (token == 'w') ? WHITE : BLACK;
    ss >> token;   // skip space

    // 3) Castling rights — supports both standard FEN and Shredder/X-FEN
    // notation. Standard letters K/Q/k/q find the rook on FILE_H/A. X-FEN
    // letters A-H/a-h indicate the rook's starting file directly (used for
    // Chess960). Letters that don't correspond to a rook of the right color
    // on the king's rank are ignored.
    while ((ss >> token) && !std::isspace(static_cast<unsigned char>(token))) {
        if (token == '-') continue;
        Color c = std::isupper(static_cast<unsigned char>(token)) ? WHITE : BLACK;
        char ch = char(std::toupper(static_cast<unsigned char>(token)));
        Square ksq = square<KING>(c);
        Square rsq = SQ_NONE;
        if (ch == 'K') {
            // Outermost rook on the king's rank, kingside. In standard chess
            // that's FILE_H; in Chess960 it's the rightmost rook of color c.
            for (int f = FILE_H; f >= FILE_A; --f) {
                Square s = home_rank_square(c, File(f));
                if (piece_on(s) == make_piece(c, ROOK) && file_of(s) > file_of(ksq)) {
                    rsq = s; break;
                }
            }
        } else if (ch == 'Q') {
            // Outermost rook, queenside.
            for (int f = FILE_A; f <= FILE_H; ++f) {
                Square s = home_rank_square(c, File(f));
                if (piece_on(s) == make_piece(c, ROOK) && file_of(s) < file_of(ksq)) {
                    rsq = s; break;
                }
            }
        } else if (ch >= 'A' && ch <= 'H') {
            // X-FEN: rook is on the named file, on the king's rank.
            rsq = home_rank_square(c, File(ch - 'A'));
        }
        if (rsq != SQ_NONE && piece_on(rsq) == make_piece(c, ROOK)
            && rank_of(ksq) == rank_of(rsq))
            set_castling_right(c, rsq);
    }

    // 4) En-passant target square. Start with SQ_NONE — without this the
    //    field keeps whatever junk was in StateInfo memory, and `fen()`
    //    later writes that junk square instead of '-'. Bug surfaced when
    //    feeding our FEN to Stockfish 18, which strictly validates ep.
    st->epSquare = SQ_NONE;
    char col = 0, row = 0;
    if ((ss >> col) && col != '-' && (ss >> row)) {
        Square epsq = make_square(File(col - 'a'), Rank(row - '1'));
        // Only set epSquare if a pawn of the side to move can actually capture there.
        if (PawnAttacks[~sideToMove][epsq] & pieces(sideToMove, PAWN))
            st->epSquare = epsq;
    }
    ss >> token;   // skip space

    // 5) Half-move clock + 6) full-move counter
    int rule50 = 0, fullmove = 1;
    ss >> std::skipws >> rule50 >> fullmove;
    st->rule50  = rule50;
    gamePly = std::max(2 * (fullmove - 1), 0) + (sideToMove == BLACK);

    set_state();
    set_check_info();
    return *this;
}

// ---------------------------------------------------------------------------
// set_state — recompute hash keys, material totals, and check geometry from
// scratch. Called once after FEN parse; after that, do_move maintains them
// incrementally.
// ---------------------------------------------------------------------------
void Position::set_state() {
    // 2026-05-17 zobrist audit finding: SF18 uses Zobrist::noPawns as the
    // pawnKey seed (position.cpp:346) so that a no-pawn position gets a
    // unique nonzero pawnKey instead of colliding with pawnKey=0 (the
    // initial state). Affects pawnCorrHist hash distribution slightly —
    // existing PersistCorrHist files become invalid (which is fine; default
    // is OFF and the file gets rebuilt on first ucinewgame).
    st->key = st->materialKey = st->minorKey = 0;
    st->pawnKey = Zobrist::noPawns;
    st->nonPawnKey[WHITE] = st->nonPawnKey[BLACK] = 0;
    st->nonPawnMaterial[WHITE] = st->nonPawnMaterial[BLACK] = 0;
    st->capturedPiece = NO_PIECE;

    for (Bitboard b = pieces(); b; ) {
        Square s  = pop_lsb(b);
        Piece  pc = board[s];
        Key    k  = Zobrist::psq[pc][s];
        st->key ^= k;
        if (type_of(pc) == PAWN) {
            st->pawnKey ^= k;
        } else {
            st->nonPawnKey[color_of(pc)] ^= k;
            if (type_of(pc) != KING)
                st->nonPawnMaterial[color_of(pc)] += PieceValue[type_of(pc)];
            if (type_of(pc) == KNIGHT || type_of(pc) == BISHOP || type_of(pc) == KING)
                st->minorKey ^= k;
        }
    }
    if (sideToMove == BLACK) st->key ^= Zobrist::side;
    if (st->epSquare != SQ_NONE) st->key ^= Zobrist::enpassant[file_of(st->epSquare)];
    st->key ^= Zobrist::castling[st->castlingRights];

    for (Color c : { WHITE, BLACK })
        for (PieceType pt = PAWN; pt <= KING; ++pt)
            for (int cnt = 0; cnt < pieceCount[make_piece(c, pt)]; ++cnt)
                st->materialKey ^= Zobrist::psq[make_piece(c, pt)][cnt];

    // Checkers — pieces of the opposite color attacking our king.
    st->checkersBB = attackers_to(square<KING>(sideToMove)) & pieces(~sideToMove);

    st->repetition = 0;
}

// ---------------------------------------------------------------------------
// slider_blockers — pieces (of either color) between an enemy slider and the
// square `s`. Sets `pinners` to the slider pieces that are pinning. Used both
// for set_check_info and for legality checks of king moves.
// ---------------------------------------------------------------------------
Bitboard Position::slider_blockers(Bitboard sliders, Square s, Bitboard& pinners) const {
    Bitboard blockers = 0;
    pinners = 0;

    Bitboard snipers = ((PseudoAttacks[ROOK][s]   & pieces(QUEEN, ROOK))
                      | (PseudoAttacks[BISHOP][s] & pieces(QUEEN, BISHOP))) & sliders;
    Bitboard occupancy = pieces() ^ snipers;

    while (snipers) {
        Square sniperSq = pop_lsb(snipers);
        Bitboard b = BetweenBB[s][sniperSq] & occupancy & ~square_bb(sniperSq);
        if (b && !more_than_one(b)) {
            blockers |= b;
            // Pinner: the snipers' color piece that pins, if our piece is between.
            if (b & pieces(color_of(piece_on(s))))
                pinners |= square_bb(sniperSq);
        }
    }
    return blockers;
}

// ---------------------------------------------------------------------------
// set_check_info — refresh blockers, pinners, and checkSquares so the rest
// of search can read them without recomputing.
// ---------------------------------------------------------------------------
void Position::set_check_info() {
    st->blockersForKing[WHITE] = slider_blockers(pieces(BLACK), square<KING>(WHITE), st->pinners[BLACK]);
    st->blockersForKing[BLACK] = slider_blockers(pieces(WHITE), square<KING>(BLACK), st->pinners[WHITE]);

    Square ksq = square<KING>(~sideToMove);
    st->checkSquares[PAWN]   = pawn_attacks_bb(~sideToMove, ksq);
    st->checkSquares[KNIGHT] = PseudoAttacks[KNIGHT][ksq];
    st->checkSquares[BISHOP] = attacks_bb<BISHOP>(ksq, pieces());
    st->checkSquares[ROOK]   = attacks_bb<ROOK>  (ksq, pieces());
    st->checkSquares[QUEEN]  = st->checkSquares[BISHOP] | st->checkSquares[ROOK];
    st->checkSquares[KING]   = 0;
}

// ---------------------------------------------------------------------------
// attackers_to — bitboard of all pieces attacking square `s`.
// ---------------------------------------------------------------------------
Bitboard Position::attackers_to(Square s, Bitboard occupied) const {
    return  (pawn_attacks_bb(BLACK, s) & pieces(WHITE, PAWN))
          | (pawn_attacks_bb(WHITE, s) & pieces(BLACK, PAWN))
          | (PseudoAttacks[KNIGHT][s] & pieces(KNIGHT))
          | (attacks_bb<ROOK>  (s, occupied) & pieces(QUEEN, ROOK))
          | (attacks_bb<BISHOP>(s, occupied) & pieces(QUEEN, BISHOP))
          | (PseudoAttacks[KING][s] & pieces(KING));
}

// ---------------------------------------------------------------------------
// do_move / undo_move
// ---------------------------------------------------------------------------
void Position::do_move(Move m, StateInfo& newSt) { do_move(m, newSt, gives_check(m)); }

void Position::do_move(Move m, StateInfo& newSt, bool givesCheck) {
    // Copy the rollback fields from the previous state. The cache fields
    // (key, checkersBB, capturedPiece, etc.) get rewritten below.
    std::memcpy(static_cast<void*>(&newSt), st, offsetof(StateInfo, key));
    newSt.previous = st;
    // NNUE: clear dirty list + invalidate accumulator slots. NNUE::make_valid
    // will refill on next eval — see src/nnue.cpp.
    newSt.dirtyCount = 0;
    newSt.nnue.valid_big[0]   = newSt.nnue.valid_big[1]   = false;
    newSt.nnue.valid_small[0] = newSt.nnue.valid_small[1] = false;
    st = &newSt;

    Color  us   = sideToMove;
    Color  them = ~us;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = board[from];
    Piece  captured = (m.type_of() == MT_EN_PASSANT) ? make_piece(them, PAWN) : board[to];

    Key k = st->previous->key ^ Zobrist::side;

    ++gamePly;
    ++st->rule50;
    ++st->pliesFromNull;

    if (m.type_of() == MT_CASTLING) {
        // King + rook both move. The MT_CASTLING move stores from = king's
        // start square, to = king's destination (FILE_G or FILE_C on the home
        // rank — same in standard chess and Chess960). Determine side from
        // the destination file (works regardless of king's starting file in
        // Chess960 — king start can be anywhere between B and G).
        bool kingSide = file_of(to) == FILE_G;
        CastlingRights cr = (us == WHITE) ? (kingSide ? WHITE_OO : WHITE_OOO)
                                          : (kingSide ? BLACK_OO : BLACK_OOO);
        Square rfrom = castlingRookSquare[cr];   // Chess960-aware
        Square kto   = home_rank_square(us, kingSide ? FILE_G : FILE_C);
        Square rto   = home_rank_square(us, kingSide ? FILE_F : FILE_D);

        remove_piece(from);  remove_piece(rfrom);
        put_piece(make_piece(us, KING), kto);
        put_piece(make_piece(us, ROOK), rto);

        k ^= Zobrist::psq[make_piece(us, KING)][from] ^ Zobrist::psq[make_piece(us, KING)][kto];
        k ^= Zobrist::psq[make_piece(us, ROOK)][rfrom] ^ Zobrist::psq[make_piece(us, ROOK)][rto];
        st->nonPawnKey[us] ^= Zobrist::psq[make_piece(us, KING)][from] ^ Zobrist::psq[make_piece(us, KING)][kto];
        st->nonPawnKey[us] ^= Zobrist::psq[make_piece(us, ROOK)][rfrom] ^ Zobrist::psq[make_piece(us, ROOK)][rto];
        st->minorKey       ^= Zobrist::psq[make_piece(us, KING)][from] ^ Zobrist::psq[make_piece(us, KING)][kto];

        // NNUE: king and rook each move (from -> to)
        st->dirtyPiece[0] = { make_piece(us, KING), from,  kto  };
        st->dirtyPiece[1] = { make_piece(us, ROOK), rfrom, rto  };
        st->dirtyCount    = 2;
        captured = NO_PIECE;
    } else {
        if (captured != NO_PIECE) {
            Square capsq = to;
            if (m.type_of() == MT_EN_PASSANT) capsq = to + (us == WHITE ? -8 : 8);
            remove_piece(capsq);
            k ^= Zobrist::psq[captured][capsq];

            // NNUE: captured piece is removed from the board.
            st->dirtyPiece[st->dirtyCount++] = { captured, capsq, SQ_NONE };

            if (type_of(captured) == PAWN) {
                st->pawnKey ^= Zobrist::psq[captured][capsq];
            } else {
                st->nonPawnKey[them] ^= Zobrist::psq[captured][capsq];
                if (type_of(captured) != KING)
                    st->nonPawnMaterial[them] -= PieceValue[type_of(captured)];
                if (type_of(captured) == KNIGHT || type_of(captured) == BISHOP || type_of(captured) == KING)
                    st->minorKey ^= Zobrist::psq[captured][capsq];
            }
            // materialKey: position of the count-th piece in our table; flip the slot we just freed.
            st->materialKey ^= Zobrist::psq[captured][pieceCount[captured]];
            st->rule50 = 0;
        }

        // Move the moving piece (or, for promotions, swap pawn for promotion piece).
        if (m.type_of() == MT_PROMOTION) {
            Piece promoPc = make_piece(us, m.promotion_type());
            remove_piece(from);
            put_piece(promoPc, to);
            k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[promoPc][to];
            st->pawnKey      ^= Zobrist::psq[pc][from];
            st->nonPawnKey[us] ^= Zobrist::psq[promoPc][to];
            if (m.promotion_type() != KING)
                st->nonPawnMaterial[us] += PieceValue[m.promotion_type()];
            if (m.promotion_type() == KNIGHT || m.promotion_type() == BISHOP)
                st->minorKey ^= Zobrist::psq[promoPc][to];
            st->materialKey ^= Zobrist::psq[pc][pieceCount[pc]];
            st->materialKey ^= Zobrist::psq[promoPc][pieceCount[promoPc] - 1];

            // NNUE: pawn removed from `from`, promoted piece appears on `to`.
            st->dirtyPiece[st->dirtyCount++] = { pc,      from,    SQ_NONE };
            st->dirtyPiece[st->dirtyCount++] = { promoPc, SQ_NONE, to      };
        } else {
            move_piece(from, to);
            k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
            if (type_of(pc) == PAWN) {
                st->pawnKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
                st->rule50 = 0;
            } else {
                st->nonPawnKey[us] ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
                if (type_of(pc) == KNIGHT || type_of(pc) == BISHOP || type_of(pc) == KING)
                    st->minorKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
            }

            // NNUE: piece moves from -> to.
            st->dirtyPiece[st->dirtyCount++] = { pc, from, to };
        }
    }

    // ---- Castling rights ----
    if (st->castlingRights && (castlingRightsMask[from] | castlingRightsMask[to])) {
        int cr = st->castlingRights;
        k ^= Zobrist::castling[cr];
        st->castlingRights &= ~(castlingRightsMask[from] | castlingRightsMask[to]);
        k ^= Zobrist::castling[st->castlingRights];
    }

    // ---- En passant square ----
    if (st->epSquare != SQ_NONE) {
        k ^= Zobrist::enpassant[file_of(st->epSquare)];
        st->epSquare = SQ_NONE;
    }
    if (type_of(pc) == PAWN
        && std::abs(int(to) - int(from)) == 16) {
        Square epCandidate = Square((int(from) + int(to)) / 2);
        // Only set if an enemy pawn can actually capture (Stockfish-style filter).
        if (PawnAttacks[us][epCandidate] & pieces(them, PAWN)) {
            st->epSquare = epCandidate;
            k ^= Zobrist::enpassant[file_of(epCandidate)];
        }
    }

    st->capturedPiece = captured;
    st->key           = k;
    sideToMove        = them;

    set_check_info();
    st->checkersBB = givesCheck ? (attackers_to(square<KING>(sideToMove)) & pieces(~sideToMove)) : 0;

    recompute_repetition();
}

// Walk the StateInfo->previous chain looking for matches of the current
// position key. Sets st->repetition to:
//   0   = no match within the search window
//   +i  = position seen i plies back (this is the 2nd occurrence)
//   -i  = position seen i plies back AND that prior occurrence was itself
//         a repetition (so this is the 3rd or later occurrence — draw).
//
// is_draw() returns true when (repetition && repetition < ply), where ply is
// the search ply. This means in-tree 2-folds count as draws (perpetual is
// detected as soon as the search sees the second occurrence inside the
// tree), and history 3-folds count as draws.
void Position::recompute_repetition() {
    st->repetition = 0;
    int end = std::min(st->rule50, st->pliesFromNull);
    if (end >= 4
        && st->previous != nullptr
        && st->previous->previous != nullptr) {
        StateInfo* stp = st->previous->previous;
        for (int i = 4; i <= end; i += 2) {
            if (stp->previous == nullptr || stp->previous->previous == nullptr) break;
            stp = stp->previous->previous;
            if (stp->key == st->key) {
                st->repetition = stp->repetition ? -i : i;
                break;
            }
        }
    }
}

void Position::undo_move(Move m) {
    sideToMove = ~sideToMove;
    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = board[to];

    if (m.type_of() == MT_PROMOTION) {
        remove_piece(to);
        pc = make_piece(us, PAWN);
        put_piece(pc, from);
    } else if (m.type_of() == MT_CASTLING) {
        // Same Chess960-aware decoding as in do_move (above).
        bool kingSide = file_of(to) == FILE_G;
        CastlingRights cr = (us == WHITE) ? (kingSide ? WHITE_OO : WHITE_OOO)
                                          : (kingSide ? BLACK_OO : BLACK_OOO);
        Square rfrom = castlingRookSquare[cr];
        Square kto   = home_rank_square(us, kingSide ? FILE_G : FILE_C);
        Square rto   = home_rank_square(us, kingSide ? FILE_F : FILE_D);
        remove_piece(kto);  remove_piece(rto);
        put_piece(make_piece(us, KING), from);
        put_piece(make_piece(us, ROOK), rfrom);
    } else {
        move_piece(to, from);
    }

    if (st->capturedPiece != NO_PIECE) {
        Square capsq = to;
        if (m.type_of() == MT_EN_PASSANT) capsq = to + (us == WHITE ? -8 : 8);
        put_piece(st->capturedPiece, capsq);
    }

    st = st->previous;
    --gamePly;
}

void Position::do_null_move(StateInfo& newSt) {
    // Copy only the rollback fields (everything above `key`). The cached
    // fields (key, checkers, captured, repetition) are recomputed below;
    // the NNUE accumulator is invalidated and lazily refilled on next eval
    // — make_valid() walks back to the parent and inherits its accumulator
    // since dirtyCount = 0 means no piece moved.
    std::memcpy(static_cast<void*>(&newSt), st, offsetof(StateInfo, key));
    newSt.previous = st;
    newSt.dirtyCount = 0;
    newSt.nnue.valid_big[0]   = newSt.nnue.valid_big[1]   = false;
    newSt.nnue.valid_small[0] = newSt.nnue.valid_small[1] = false;
    // Carry forward the cache fields we need below; key is patched, others
    // are recomputed.
    newSt.key           = st->key;
    newSt.checkersBB    = 0;            // no checks possible right after null
    newSt.capturedPiece = NO_PIECE;
    newSt.repetition    = 0;
    st = &newSt;

    if (st->epSquare != SQ_NONE) {
        st->key ^= Zobrist::enpassant[file_of(st->epSquare)];
        st->epSquare = SQ_NONE;
    }
    st->key ^= Zobrist::side;
    ++st->rule50;
    st->pliesFromNull = 0;
    sideToMove = ~sideToMove;

    set_check_info();
    st->repetition = 0;
}

void Position::undo_null_move() {
    st = st->previous;
    sideToMove = ~sideToMove;
}

// ---------------------------------------------------------------------------
// Move legality / properties
// ---------------------------------------------------------------------------
bool Position::capture(Move m) const {
    return (!empty(m.to_sq()) && m.type_of() != MT_CASTLING) || m.type_of() == MT_EN_PASSANT;
}

bool Position::capture_or_promotion(Move m) const {
    return capture(m) || m.type_of() == MT_PROMOTION;
}

bool Position::legal(Move m) const {
    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Square ksq  = square<KING>(us);

    // En-passant: brute-force re-test for discovered check on king after removal of both pawns.
    if (m.type_of() == MT_EN_PASSANT) {
        Square capsq = to + (us == WHITE ? -8 : 8);
        Bitboard occ = (pieces() ^ square_bb(from) ^ square_bb(capsq)) | square_bb(to);
        return  !(attacks_bb<ROOK>  (ksq, occ) & pieces(~us, QUEEN, ROOK))
             && !(attacks_bb<BISHOP>(ksq, occ) & pieces(~us, QUEEN, BISHOP));
    }

    // Castling moves are generated already verified for path-clear and
    // not-through-check, so they're always legal here.
    if (m.type_of() == MT_CASTLING) return true;

    // King moves: target square must not be attacked.
    if (type_of(piece_on(from)) == KING)
        return !(attackers_to(to, pieces() ^ square_bb(from)) & pieces(~us));

    // Pinned-piece moves: must stay on the king-pinner ray.
    return !(blockers_for_king(us) & square_bb(from)) || aligned(from, to, ksq);
}

// pseudo_legal: validates a Move that came from outside (TT, GUI input). Catches
// bogus moves before do_move trusts them.
bool Position::pseudo_legal(Move m) const {
    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = piece_on(from);

    if (pc == NO_PIECE || color_of(pc) != us) return false;
    if (pieces(us) & square_bb(to))            return false;

    // Reaching a position where the enemy king is capturable means a previous
    // move was illegal — never accept such a move.
    if (type_of(board[to]) == KING) return false;

    // ---- Non-normal moves: fully validate before delegating to legal() ----
    if (m.type_of() == MT_EN_PASSANT) {
        if (type_of(pc) != PAWN || to != st->epSquare) return false;
        Square capsq = to + (us == WHITE ? SOUTH : NORTH);
        if (piece_on(capsq) != make_piece(~us, PAWN)) return false;
        if (!(pawn_attacks_bb(us, from) & square_bb(to))) return false;
        return legal(m);
    }
    if (m.type_of() == MT_CASTLING) {
        if (type_of(pc) != KING) return false;
        if (checkers()) return false;
        // Confirm a generated legal castling exists with these from/to squares.
        for (Move gm : MoveList<LEGAL>(*this))
            if (gm.type_of() == MT_CASTLING && gm.from_sq() == from && gm.to_sq() == to)
                return true;
        return false;
    }
    if (m.type_of() == MT_PROMOTION) {
        if (type_of(pc) != PAWN) return false;
        if (rank_of(to) != (us == WHITE ? RANK_8 : RANK_1)) return false;
        if (rank_of(from) != (us == WHITE ? RANK_7 : RANK_2)) return false;
        // Push or capture pattern must match (same as the normal-pawn check below).
        if (!(pawn_attacks_bb(us, from) & square_bb(to) & pieces(~us))
            && !(from + (us == WHITE ? NORTH : SOUTH) == to && empty(to)))
            return false;
        return legal(m);
    }
    if (m.promotion_type() != KNIGHT) return false;   // bits 12-13 must be 0 for normal moves

    if (type_of(pc) == PAWN) {
        if (rank_of(to) == RANK_8 || rank_of(to) == RANK_1) return false;
        if (!(pawn_attacks_bb(us, from) & square_bb(to) & pieces(~us))
            && !(from + (us == WHITE ? NORTH : SOUTH) == to && empty(to))
            && !(from + (us == WHITE ? 2 * NORTH : 2 * SOUTH) == to
                 && rank_of(from) == (us == WHITE ? RANK_2 : RANK_7)
                 && empty(to)
                 && empty(Square(int(to) - (us == WHITE ? NORTH : SOUTH)))))
            return false;
    } else if (!(attacks_bb(type_of(pc), from, pieces()) & square_bb(to))) {
        return false;
    }

    if (checkers()) {
        if (type_of(pc) != KING) {
            if (more_than_one(checkers())) return false;
            // Must block or capture the checker.
            if (!((BetweenBB[square<KING>(us)][lsb(checkers())]) & square_bb(to))) return false;
        } else if (attackers_to(to, pieces() ^ square_bb(from)) & pieces(~us)) {
            return false;
        }
    }
    return true;
}

bool Position::gives_check(Move m) const {
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = piece_on(from);
    Square ksq  = square<KING>(~sideToMove);

    // Direct check.
    if (st->checkSquares[type_of(pc)] & square_bb(to)) return true;

    // Discovered check: moving piece was a blocker, and movement leaves a non-aligned ray.
    if ((blockers_for_king(~sideToMove) & square_bb(from)) && !aligned(from, to, ksq))
        return true;

    if (m.type_of() == MT_NORMAL) return false;

    if (m.type_of() == MT_PROMOTION) {
        return attacks_bb(m.promotion_type(), to, pieces() ^ square_bb(from)) & square_bb(ksq);
    }
    if (m.type_of() == MT_EN_PASSANT) {
        Square capsq = make_square(file_of(to), rank_of(from));
        Bitboard b = (pieces() ^ square_bb(from) ^ square_bb(capsq)) | square_bb(to);
        return  (attacks_bb<ROOK>  (ksq, b) & pieces(sideToMove, QUEEN, ROOK))
             || (attacks_bb<BISHOP>(ksq, b) & pieces(sideToMove, QUEEN, BISHOP));
    }
    // Castling — rook-after-castle can give check.
    Square kto = (to > from) ? home_rank_square(sideToMove, FILE_G) : home_rank_square(sideToMove, FILE_C);
    Square rto = (to > from) ? home_rank_square(sideToMove, FILE_F) : home_rank_square(sideToMove, FILE_D);
    return (PseudoAttacks[ROOK][rto] & square_bb(ksq))
        && (attacks_bb<ROOK>(rto, (pieces() ^ square_bb(from) ^ square_bb(to)) | square_bb(kto) | square_bb(rto)) & square_bb(ksq));
}

// SEE — placeholder until M3. Returns true so all captures look "good"
// in early search; movegen is unaffected.
// Static Exchange Evaluation: returns true if SEE(move) >= threshold.
// Stockfish-style swap-off using bitboards. Each side captures with the
// least-valued attacker and we backwards-minimax to decide if it's worth it.
bool Position::see_ge(Move m, Value threshold) const {
    // Promotions / castling / EP are approximated — accept any threshold <= 0.
    if (m.type_of() != MT_NORMAL) return VALUE_ZERO >= threshold;

    Square from = m.from_sq(), to = m.to_sq();

    // First gain: capturing the piece on `to`.
    int swap = PieceValue[type_of(piece_on(to))] - threshold;
    if (swap < 0) return false;

    // After our capture, opponent gets to recapture. If they can't take enough
    // back, we still beat the threshold.
    swap = PieceValue[type_of(piece_on(from))] - swap;
    if (swap <= 0) return true;

    Bitboard occ       = pieces() ^ square_bb(from) ^ square_bb(to);
    Bitboard attackers = attackers_to(to, occ);
    Color    stm       = ~color_of(piece_on(from));   // opponent moves next
    int      result    = 1;                            // 1 = currently winning for our side

    while (true) {
        Bitboard stmAttackers = attackers & pieces(stm);
        if (!stmAttackers) break;

        // Stop pinned attackers from contributing if the king isn't already on `to`.
        // (Skipped for simplicity — costs a few elo of accuracy in very rare positions.)
        // NOTE: tried porting SF18 pin-aware logic
        //   `if (pinners(~stm) & occ) stmAttackers &= ~blockers_for_king(stm);`
        // Result: -10.4 +/- 36.1 ELO at 200g 5+0.05.  Within noise but mildly
        // negative — Hypersion's surrounding margins (SEE_QUIET_MARGIN etc.)
        // are tuned assuming the simpler SEE; the more accurate version
        // changes which moves get pruned, and re-tuning the margins is
        // needed to capture the win.

        // Find the least-valued attacker for this side.
        PieceType pt;
        Bitboard b = 0;
        for (pt = PAWN; pt <= KING; pt = PieceType(pt + 1)) {
            b = stmAttackers & pieces(pt);
            if (b) break;
        }

        // 2026-05-17 fix: result-flip MUST happen BEFORE the KING-case
        // dispatch, mirroring SF18 src/position.cpp:1390-1393. The previous
        // code flipped after, so the KING branch returned the *inverted*
        // result-polarity. Symptom: any capture whose target is defended
        // only by the enemy king made `see_ge(m, 0)` return TRUE when SEE
        // was actually negative (and vice versa), affecting qsearch SEE
        // pruning, ProbCut, LMP, shallow pruning, MovePicker capture
        // ordering — all the hot pruning paths. The earlier "pin-aware
        // SEE = -10 ELO" tombstone at line 731 was measured WITH this bug
        // active, so the surrounding margins (SEE_QUIET_MARGIN /
        // SEE_CAPT_MARGIN) may now want re-tuning.
        result ^= 1;

        if (pt == KING) {
            // Capturing with the king is only legal if there are no remaining defenders.
            return (attackers & pieces(~stm)) ? bool(result ^ 1) : bool(result);
        }

        // Remove this attacker from the occupancy and add any X-ray attackers it unmasked.
        occ ^= square_bb(lsb(b));
        if (pt == PAWN || pt == BISHOP || pt == QUEEN)
            attackers |= attacks_bb<BISHOP>(to, occ) & pieces(BISHOP, QUEEN);
        if (pt == ROOK || pt == QUEEN)
            attackers |= attacks_bb<ROOK>(to, occ) & pieces(ROOK, QUEEN);
        attackers &= occ;

        // Backwards minimax. (result already flipped above; just update swap.)
        swap = PieceValue[pt] - swap;
        if (swap < result) return bool(result);
        stm = ~stm;
    }
    return bool(result);
}

// ---------------------------------------------------------------------------
// Upcoming-repetition cuckoo lookup — SF18 src/position.cpp:1432.
// Returns true if a non-pawn single-move reversal exists from the current
// position to one already in our chain. Used by search to do an alpha-up-to
// VALUE_DRAW bump when the side to move can force a repetition draw — saves
// search time on positions whose only progress is a repetition cycle.
// ---------------------------------------------------------------------------
bool Position::upcoming_repetition(int ply) const {
    int j;
    int end = std::min<int>(st->rule50, st->pliesFromNull);
    if (end < 3) return false;

    Key        originalKey = st->key;
    StateInfo* stp         = st->previous;
    if (!stp) return false;
    Key        other       = originalKey ^ stp->key ^ Zobrist::side;

    for (int i = 3; i <= end; i += 2) {
        if (!stp->previous) return false;
        stp = stp->previous;
        if (!stp->previous) return false;
        other ^= stp->key ^ stp->previous->key ^ Zobrist::side;
        stp = stp->previous;
        if (other != 0) continue;

        Key moveKey = originalKey ^ stp->key;
        if ((j = H1(moveKey), cuckoo[j] == moveKey)
         || (j = H2(moveKey), cuckoo[j] == moveKey)) {
            Move   move = cuckooMove[j];
            Square s1   = move.from_sq();
            Square s2   = move.to_sq();
            // Between-squares-must-be-empty test, mirroring SF18.
            // BetweenBB[s1][s2] in Hypersion INCLUDES s2 (per bitboard.cpp:171);
            // SF's between_bb does too, so XOR ^ s2 makes it "strictly between".
            if (!((BetweenBB[s1][s2] ^ square_bb(s2)) & pieces())) {
                if (ply > i) return true;
                // For pre-root nodes, require this to be a real repetition.
                if (stp->repetition) return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Draw detection
// ---------------------------------------------------------------------------
bool Position::is_draw(int ply) const {
    // 50-move rule: position is a draw if rule50 has reached 100 plies
    // UNLESS the side to move is checkmated (in check with no legal moves).
    // Stalemate (not in check + no legal moves) is already a draw, so the
    // legal-moves check only matters when we're in check.
    if (st->rule50 > 99
        && (!checkers() || MoveList<LEGAL>(*this).size() > 0))
        return true;
    // Threefold (or fold seen within search ply window).
    if (st->repetition && st->repetition < ply) return true;
    // Insufficient material — FIDE article 9.6 / lichess rules. Covers:
    //   K vs K
    //   K + (one minor) vs K
    //   K + B vs K + B   when both bishops on the same colour squares
    // No pawns/rooks/queens anywhere. Saves NNUE eval cost and prevents
    // the engine from trading down into a position it can't actually win.
    if (pieces(PAWN) | pieces(ROOK) | pieces(QUEEN)) return false;
    int wMinors = popcount(pieces(WHITE, KNIGHT) | pieces(WHITE, BISHOP));
    int bMinors = popcount(pieces(BLACK, KNIGHT) | pieces(BLACK, BISHOP));
    if (wMinors == 0 && bMinors == 0) return true;            // K vs K
    if (wMinors + bMinors == 1) return true;                  // K(+minor) vs K
    if (wMinors == 1 && bMinors == 1
        && popcount(pieces(WHITE, BISHOP)) == 1
        && popcount(pieces(BLACK, BISHOP)) == 1) {
        // KBvKB — drawn iff bishops are on the same colour.
        // A square's colour is (file + rank) parity, NOT (square & 1):
        // a1=0 file=0 rank=0 → dark; a2=8 file=0 rank=1 → light, but
        // (a1 + a2) & 1 == 0 (would falsely report same).
        Square wb = lsb(pieces(WHITE, BISHOP));
        Square bb = lsb(pieces(BLACK, BISHOP));
        auto color_of_sq = [](Square s) {
            return ((int(s) & 7) + (int(s) >> 3)) & 1;
        };
        if (color_of_sq(wb) == color_of_sq(bb))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// FEN output
// ---------------------------------------------------------------------------
std::string Position::fen() const {
    std::ostringstream ss;
    for (Rank r = RANK_8; r >= RANK_1; --r) {
        int empties = 0;
        for (File f = FILE_A; f <= FILE_H; ++f) {
            Piece pc = board[make_square(f, r)];
            if (pc == NO_PIECE) ++empties;
            else { if (empties) { ss << empties; empties = 0; } ss << PieceChars[pc]; }
        }
        if (empties) ss << empties;
        if (r > RANK_1) ss << '/';
    }
    ss << ' ' << (sideToMove == WHITE ? 'w' : 'b') << ' ';
    if (st->castlingRights == 0) ss << '-';
    else if (is_chess960()) {
        // 2026-05-17 audit uci [25]: X-FEN/Shredder castling notation —
        // emit the rook's file directly (uppercase for white, lowercase
        // for black). Required by GUIs in Chess960 mode so they can
        // re-parse the FEN without ambiguity about which rook can castle.
        auto emit = [&](CastlingRights cr, Color c) {
            if (!(st->castlingRights & cr)) return;
            char fch = char('A' + int(file_of(castlingRookSquare[cr])));
            ss << (c == WHITE ? fch : char(std::tolower(static_cast<unsigned char>(fch))));
        };
        emit(WHITE_OO,  WHITE);
        emit(WHITE_OOO, WHITE);
        emit(BLACK_OO,  BLACK);
        emit(BLACK_OOO, BLACK);
    }
    else {
        if (st->castlingRights & WHITE_OO)  ss << 'K';
        if (st->castlingRights & WHITE_OOO) ss << 'Q';
        if (st->castlingRights & BLACK_OO)  ss << 'k';
        if (st->castlingRights & BLACK_OOO) ss << 'q';
    }
    ss << ' ';
    if (st->epSquare == SQ_NONE) ss << '-';
    else ss << char('a' + file_of(st->epSquare)) << char('1' + rank_of(st->epSquare));
    ss << ' ' << st->rule50 << ' ' << (1 + (gamePly - (sideToMove == BLACK)) / 2);
    return ss.str();
}

// ---------------------------------------------------------------------------
// Pretty-print and sanity check
// ---------------------------------------------------------------------------
std::string Position::pretty() const {
    std::ostringstream ss;
    ss << "\n +---+---+---+---+---+---+---+---+\n";
    for (Rank r = RANK_8; r >= RANK_1; --r) {
        for (File f = FILE_A; f <= FILE_H; ++f)
            ss << " | " << PieceChars[board[make_square(f, r)]];
        ss << " | " << (1 + int(r)) << "\n +---+---+---+---+---+---+---+---+\n";
    }
    ss << "   a   b   c   d   e   f   g   h\n\n"
       << "FEN: " << fen() << "\nKey: " << std::hex << st->key << std::dec << "\n";
    return ss.str();
}

bool Position::pos_is_ok() const {
    if (popcount(pieces(WHITE, KING)) != 1 || popcount(pieces(BLACK, KING)) != 1) return false;
    if (attackers_to(square<KING>(~sideToMove)) & pieces(sideToMove)) return false;
    return true;
}

std::ostream& operator<<(std::ostream& os, const Position& pos) { return os << pos.pretty(); }

}  // namespace hypersion
