// Hypersion — minimal PGN-to-(fen | result) extractor for Texel tuning.
//
// Reads a PGN file, walks each game by parsing SAN moves against Hypersion's
// own move generator, samples positions every N plies, and writes them to
// stdout (or --out FILE) in the format the tuner expects:
//
//     <fen> | <result>
//
// where <result> is the GAME outcome (1, 0.5, 0) — i.e. every position from
// a game shares the same label. That's the standard Texel formulation.
//
// SAN parsing handles:
//   * Pawn moves         e4, e5, exd5, exd5e.p.
//   * Piece moves        Nf3, Nbd7, R1e1, Qh4xe1
//   * Captures           Nxd5
//   * Promotions         e8=Q, exd8=Q+
//   * Castling           O-O, O-O-O, 0-0, 0-0-0
//   * Check/mate/NAGs    + # !? !! ?! ??  (stripped)
//   * Comments/variations    {comment}  (var)  $7  (stripped)
//
// Build:
//     make pgn_to_positions ARCH=x86-64-avx2
// Use:
//     pgn_to_positions --in 2025\ Database.pgn --out positions.txt
//                      [--every 6] [--skip-opening 8] [--skip-tail 6]
//                      [--max-records 5000000]

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/bitboard.h"
#include "../../src/movegen.h"
#include "../../src/position.h"
#include "../../src/zobrist.h"

using namespace hypersion;

namespace {

// Strip the SAN annotation suffix (+, #, !, ?, e.p.) so what remains is the
// "core" move spec.
std::string strip_san_decor(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+' || c == '#' || c == '!' || c == '?') break;
        // Strip "e.p." indicator if present
        if (c == 'e' && i + 3 < s.size() && s[i + 1] == '.' &&
            s[i + 2] == 'p' && s[i + 3] == '.') break;
        out += c;
    }
    return out;
}

// Find a legal move in `pos` that matches the SAN token. Returns Move::none()
// if no match (corrupt PGN, unsupported notation, ambiguous move, etc.).
Move match_san(const std::string& sanRaw, const Position& pos) {
    std::string san = strip_san_decor(sanRaw);
    if (san.empty()) return Move::none();

    // Castling.
    if (san == "O-O" || san == "0-0") {
        for (Move m : MoveList<LEGAL>(pos))
            if (m.type_of() == MT_CASTLING && file_of(m.to_sq()) == FILE_G)
                return m;
        return Move::none();
    }
    if (san == "O-O-O" || san == "0-0-0") {
        for (Move m : MoveList<LEGAL>(pos))
            if (m.type_of() == MT_CASTLING && file_of(m.to_sq()) == FILE_C)
                return m;
        return Move::none();
    }

    // Parse the trailing target square + optional promotion.
    PieceType promo  = NO_PIECE_TYPE;
    auto eq = san.rfind('=');
    if (eq != std::string::npos && eq + 1 < san.size()) {
        switch (san[eq + 1]) {
            case 'N': promo = KNIGHT; break;
            case 'B': promo = BISHOP; break;
            case 'R': promo = ROOK;   break;
            case 'Q': promo = QUEEN;  break;
            default: return Move::none();
        }
        san = san.substr(0, eq);
    }
    if (san.size() < 2) return Move::none();

    // Last 2 chars = target square.
    char tf = san[san.size() - 2];
    char tr = san[san.size() - 1];
    if (tf < 'a' || tf > 'h' || tr < '1' || tr > '8') return Move::none();
    Square to = make_square(File(tf - 'a'), Rank(tr - '1'));
    san = san.substr(0, san.size() - 2);

    // Optional capture marker 'x'.
    bool capture = false;
    if (!san.empty() && san.back() == 'x') {
        capture = true;
        san.pop_back();
    }

    // Optional rank/file disambiguation + leading piece char.
    PieceType pt = PAWN;
    if (!san.empty() && san[0] >= 'A' && san[0] <= 'Z') {
        switch (san[0]) {
            case 'N': pt = KNIGHT; break;
            case 'B': pt = BISHOP; break;
            case 'R': pt = ROOK;   break;
            case 'Q': pt = QUEEN;  break;
            case 'K': pt = KING;   break;
            default: return Move::none();
        }
        san = san.substr(1);
    }

    int wantFile = -1, wantRank = -1;
    for (char c : san) {
        if (c >= 'a' && c <= 'h')      wantFile = c - 'a';
        else if (c >= '1' && c <= '8') wantRank = c - '1';
    }
    (void)capture;   // already enforced via to-square match below

    // Search legal moves for the unique match.
    Move best = Move::none();
    int  matches = 0;
    for (Move m : MoveList<LEGAL>(pos)) {
        if (m.to_sq() != to) continue;
        Piece moverP = pos.piece_on(m.from_sq());
        if (type_of(moverP) != pt) continue;
        if (wantFile >= 0 && int(file_of(m.from_sq())) != wantFile) continue;
        if (wantRank >= 0 && int(rank_of(m.from_sq())) != wantRank) continue;
        if (promo != NO_PIECE_TYPE) {
            if (m.type_of() != MT_PROMOTION) continue;
            if (m.promotion_type() != promo) continue;
        } else {
            if (m.type_of() == MT_PROMOTION) continue;
        }
        best = m;
        ++matches;
    }
    return matches == 1 ? best : Move::none();
}

// Strip annotations from a chunk of move text: {comments}, (variations),
// $NAGs, leading move numbers (e.g. "12." or "12...").
std::string clean_movetext(const std::string& raw) {
    std::string out;
    int braceDepth = 0;
    int parenDepth = 0;
    size_t i = 0;
    while (i < raw.size()) {
        char c = raw[i];
        if (c == '{') { ++braceDepth; ++i; continue; }
        if (c == '}') { if (braceDepth) --braceDepth; ++i; continue; }
        if (c == '(') { ++parenDepth; ++i; continue; }
        if (c == ')') { if (parenDepth) --parenDepth; ++i; continue; }
        if (braceDepth || parenDepth) { ++i; continue; }
        if (c == '$') { while (i < raw.size() && !std::isspace((unsigned char)raw[i])) ++i; continue; }
        out += c;
        ++i;
    }
    return out;
}

bool is_result_token(const std::string& t) {
    return t == "1-0" || t == "0-1" || t == "1/2-1/2" || t == "*";
}

double result_value(const std::string& t) {
    if (t == "1-0")     return 1.0;
    if (t == "0-1")     return 0.0;
    if (t == "1/2-1/2") return 0.5;
    return -1.0;
}

}  // namespace


int main(int argc, char** argv) {
    Bitboards::init();
    Zobrist::init();
    Position::init();

    std::string in_path, out_path;
    int every       = 4;          // sample every N plies
    int skipOpen    = 6;          // skip first N plies (opening repetition)
    int skipTail    = 4;          // skip last N plies (mate / blunder zone)
    long long maxRecords = -1;
    bool decisive_only = false;   // skip drawn games (tactical-aligned dataset)
    int  min_total_plies = 0;     // skip very short games

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--in")          in_path = argv[++i];
        else if (a == "--out")         out_path = argv[++i];
        else if (a == "--every")       every    = std::atoi(argv[++i]);
        else if (a == "--skip-opening") skipOpen = std::atoi(argv[++i]);
        else if (a == "--skip-tail")    skipTail = std::atoi(argv[++i]);
        else if (a == "--max-records")  maxRecords = std::atoll(argv[++i]);
        else if (a == "--decisive-only") decisive_only = true;
        else if (a == "--min-plies")    min_total_plies = std::atoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                "Usage: pgn_to_positions --in <pgn> [--out <txt>] [--every N]\n"
                "                        [--skip-opening N] [--skip-tail N]\n"
                "                        [--max-records N] [--decisive-only]\n"
                "                        [--min-plies N]\n"
                "Each output line: <fen> | <result>  (result = 0|0.5|1).\n"
                "  --decisive-only skips drawn games (tactical-aligned dataset).\n"
                "  --min-plies skips games shorter than N total plies.\n");
            return 0;
        }
    }
    if (in_path.empty()) {
        std::fprintf(stderr, "--in <pgn> is required\n");
        return 1;
    }

    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", in_path.c_str());
        return 1;
    }
    std::ofstream outFile;
    std::ostream* out = &std::cout;
    if (!out_path.empty()) {
        outFile.open(out_path, std::ios::binary);
        if (!outFile) {
            std::fprintf(stderr, "cannot open %s for write\n", out_path.c_str());
            return 1;
        }
        out = &outFile;
    }

    std::string         line;
    std::string         resultStr;
    std::string         fenStr;
    std::string         moveBuf;
    long long           gamesProcessed = 0;
    long long           gamesGood      = 0;
    long long           records        = 0;

    auto flush_game = [&]() {
        if (resultStr.empty() || moveBuf.empty()) {
            resultStr.clear();
            fenStr.clear();
            moveBuf.clear();
            return;
        }
        double res = result_value(resultStr);
        if (res < 0.0) { resultStr.clear(); fenStr.clear(); moveBuf.clear(); return; }
        // --decisive-only: skip drawn games (result == 0.5). Drawn games
        // have noisier middlegame eval signal because the engine could
        // settle for a draw from a position with multiple paths; decisive
        // games have a clearer "this position led to a win" signal,
        // better aligned with tactical play.
        if (decisive_only && res == 0.5) {
            resultStr.clear(); fenStr.clear(); moveBuf.clear(); return;
        }

        // Honour the PGN [FEN "..."] tag — cutechess match files start every
        // game from a randomised opening FEN, so without this the SAN walker
        // immediately fails on move 1.
        const char* startFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        std::string usedFen = fenStr.empty() ? startFen : fenStr;

        Position pos;
        // StateInfo is ~5 KB (NNUE accumulator pair). 2048 × 5 KB = 10 MB on
        // the stack overflows the default 16 MB ld stack limit when the
        // second pass adds its own 2048 array below. Allocate on heap.
        std::vector<StateInfo> states(2048);
        int        ply = 0;
        pos.set(usedFen, &states[ply]);

        std::string clean = clean_movetext(moveBuf);
        std::vector<std::string> tokens;
        {
            std::string cur;
            for (char c : clean) {
                if (std::isspace((unsigned char)c)) {
                    if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                } else cur += c;
            }
            if (!cur.empty()) tokens.push_back(cur);
        }

        // Drop move-number tokens like "12.", "12..."
        std::vector<std::string> sans;
        sans.reserve(tokens.size());
        for (auto& t : tokens) {
            if (t.empty()) continue;
            if (is_result_token(t)) break;
            // "12." or "12..."
            size_t k = 0;
            while (k < t.size() && std::isdigit((unsigned char)t[k])) ++k;
            if (k > 0 && k < t.size() && t[k] == '.') {
                while (k < t.size() && (t[k] == '.' || std::isdigit((unsigned char)t[k]))) ++k;
                if (k >= t.size()) continue;
                t = t.substr(k);
            }
            sans.push_back(t);
        }
        ++gamesProcessed;
        if (sans.size() < std::size_t(skipOpen + skipTail + 1)) {
            resultStr.clear(); fenStr.clear(); moveBuf.clear(); return;
        }
        // --min-plies: skip games shorter than N total plies (drops blitz
        // crashes and engine-disconnect early-resign games that pollute
        // the tuning signal).
        if (min_total_plies > 0 && int(sans.size()) < min_total_plies) {
            resultStr.clear(); fenStr.clear(); moveBuf.clear(); return;
        }

        // Walk the game, applying each SAN move.
        bool ok = true;
        std::vector<int> samplePlies;
        for (size_t i = 0; i < sans.size(); ++i) {
            Move m = match_san(sans[i], pos);
            if (!m.is_ok()) { ok = false; break; }
            // Sample BEFORE applying — that way we capture the position
            // from which the move was actually played.
            int totalPlies = int(sans.size());
            int p = int(i);
            if (p >= skipOpen && p < totalPlies - skipTail && (p % every) == 0)
                samplePlies.push_back(p);
            pos.do_move(m, states[++ply]);
        }
        if (!ok) { resultStr.clear(); fenStr.clear(); moveBuf.clear(); return; }

        ++gamesGood;

        // Replay & emit the sampled positions.
        Position p2;
        std::vector<StateInfo> s2(2048);
        int p2ply = 0;
        p2.set(usedFen, &s2[p2ply]);
        size_t nextSample = 0;
        for (size_t i = 0; i < sans.size() && nextSample < samplePlies.size(); ++i) {
            if (int(i) == samplePlies[nextSample]) {
                *out << p2.fen() << " | "
                     << (res == 0.5 ? "0.5" : (res == 1.0 ? "1" : "0")) << '\n';
                ++records;
                ++nextSample;
                if (maxRecords > 0 && records >= maxRecords) {
                    resultStr.clear(); fenStr.clear(); moveBuf.clear();
                    return;
                }
            }
            Move m = match_san(sans[i], p2);
            if (!m.is_ok()) break;
            p2.do_move(m, s2[++p2ply]);
        }

        resultStr.clear();
        fenStr.clear();
        moveBuf.clear();
    };

    while (std::getline(in, line)) {
        if (maxRecords > 0 && records >= maxRecords) break;

        // Strip CR.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) {
            // Empty line. If we have a finished movetext block ending in a
            // result token, flush it.
            if (!resultStr.empty() && !moveBuf.empty()) {
                // Already in a movetext block — keep accumulating.
                // PGN allows blank lines INSIDE movetext though uncommon.
            }
            continue;
        }

        if (line[0] == '[') {
            // Header. If we have unflushed movetext, flush before starting
            // the next game.
            if (!moveBuf.empty()) flush_game();

            // Parse [Result "..."] and [FEN "..."] specifically.
            const char* p = line.c_str();
            if (std::strncmp(p, "[Result ", 8) == 0) {
                const char* q = std::strchr(p, '"');
                if (q) {
                    const char* r = std::strchr(q + 1, '"');
                    if (r) resultStr.assign(q + 1, r);
                }
            } else if (std::strncmp(p, "[FEN ", 5) == 0) {
                const char* q = std::strchr(p, '"');
                if (q) {
                    const char* r = std::strchr(q + 1, '"');
                    if (r) fenStr.assign(q + 1, r);
                }
            }
            continue;
        }

        // Move text line.
        if (!moveBuf.empty()) moveBuf += ' ';
        moveBuf += line;

        // Flush if the line ends with a result token (game over).
        for (const char* tok : {"1-0", "0-1", "1/2-1/2", "*"}) {
            size_t L = std::strlen(tok);
            if (moveBuf.size() >= L &&
                moveBuf.compare(moveBuf.size() - L, L, tok) == 0) {
                flush_game();
                break;
            }
        }
    }
    if (!moveBuf.empty()) flush_game();

    std::fprintf(stderr,
                 "games seen: %lld   games used: %lld   records emitted: %lld\n",
                 gamesProcessed, gamesGood, records);
    return 0;
}
