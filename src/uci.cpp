#include "uci.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "book.h"
#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "nnue.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "syzygy.h"
#include "tt.h"

namespace hypersion::UCI {

namespace {

constexpr const char* StartFEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

Position  pos;
StateInfo states[MAX_GAME_PLIES];
int       statePly = 0;

// UCI options. Stored as plain values; setoption updates them and applies side-effects.
struct {
    int  hashMB       = 64;          // bumped from 16 — helps slow TC, neutral at fast
    int  threads      = 2;            // lazy-SMP works (verified), 2 is a safe default
    int  multiPV      = 1;
    int  moveOverhead = 30;          // safe for cutechess; lichess users should set 100-300
    int  skillLevel   = 20;          // 0 = weakest, 20 = full strength
    bool limitStrength= false;
    int  uciElo       = 1500;
    bool ownBook      = true;
    bool bookBest     = false;
    bool analyseMode  = false;
    bool showWDL      = false;
    int  contempt     = 0;
    std::string bookFile = "Perfect2023.bin";
    std::string evalFile      = "nn-c288c895ea92.nnue";   // SF18 big default
    std::string evalFileSmall = "nn-37f18f62d772.nnue";   // SF18 small default
    std::string syzygyPath = "<empty>";
    // Opponent-aware strength matching. When matchOpponent=true and the GUI
    // sends UCI_Opponent (lichess-bot does this automatically), Hypersion
    // auto-sets UCI_LimitStrength=true and UCI_Elo to roughly the opponent's
    // level — slightly BELOW for beginners (so a 600 player can actually
    // win) and slightly above for experts (so they get a real fight).
    //
    // Updated offset curve (user feedback: bot was too strong even at +0
    // because Hypersion's calibration is approximate at low ELOs):
    //     <800       -100  (very low: bot plays well below opponent — they win)
    //     800-1199   -50   (beginner: bot plays slightly weaker)
    //     1200-1599   0    (intermediate: exact match)
    //     1600-1999  +25   (mid: slight stretch goal)
    //     2000-2399  +50   (advanced: visible challenge)
    //     >=2400     +75   (expert+: small edge for the engine)
    //
    // Final target clamped to [matchFloor=500, matchCeiling=3200].
    bool matchOpponent = false;
    int  matchFloor    = 500;
    int  matchCeiling  = 3200;
    // Per-game rated flag, set by lichess-bot before each game. When the
    // game is rated, we override matchOpponent and play at full strength
    // regardless of opponent rating — rated games count for ELO and
    // shouldn't be deliberately weakened.
    bool gameRated     = false;
} Options;

// Opponent matching offset. Below 1200 the bot plays BELOW the opponent's
// rating to give beginners a real chance to win; above 1600 the bot adds
// a small stretch-goal offset for experts.
inline int opponent_match_offset(int rating) {
    if (rating <  800) return -100;   // very-low: bot 100 below opp
    if (rating < 1200) return  -50;   // beginner: bot 50 below opp
    if (rating < 1600) return    0;   // intermediate: exact match
    if (rating < 2000) return  +25;
    if (rating < 2400) return  +50;
    return                      +75;
}

void apply_hash() { TT.resize(std::max(1, Options.hashMB)); }

void reset_position() { statePly = 0; pos.set(StartFEN, &states[statePly++]); }

Move parse_uci_move(const std::string& str, const Position& p) {
    if (str.length() < 4) return Move::none();
    for (Move m : MoveList<LEGAL>(p)) {
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
        if (s == str) return m;
    }
    return Move::none();
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------
void cmd_uci() {
    std::cout << "id name " << ENGINE_NAME << ' ' << ENGINE_VERSION << '\n'
              << "id author " << ENGINE_AUTHOR << '\n'
              << "option name Hash type spin default 64 min 1 max 65536\n"
              << "option name Threads type spin default 2 min 1 max 1024\n"
              << "option name MultiPV type spin default 1 min 1 max 256\n"
              // Move Overhead default kept at 30 ms — bumping it ate the
              // increment at TC 10+0.1 in tournament play (-26 ELO measured).
              // Lichess users with real network latency should override to
              // 100-300 ms via UCI option or the framework config.
              << "option name Move Overhead type spin default 30 min 0 max 5000\n"
              // UCI_Variant deliberately NOT advertised as a combo option:
              // python-chess (used by lichess-bot) sends `setoption name
              // UCI_Variant value chess` for standard chess, but a strict
              // `var standard` combo rejects that. Engines that don't
              // declare UCI_Variant at all are accepted as standard-only.
              << "option name Clear Hash type button\n"
              << "option name Ponder type check default false\n"
              << "option name OwnBook type check default true\n"
              << "option name BookFile type string default Perfect2023.bin\n"
              << "option name BookBestMove type check default false\n"
              << "option name EvalFile type string default nn-c288c895ea92.nnue\n"
              << "option name EvalFileSmall type string default nn-37f18f62d772.nnue\n"
              << "option name SyzygyPath type string default <empty>\n"
              << "option name SyzygyProbeDepth type spin default 1 min 1 max 100\n"
              << "option name Syzygy50MoveRule type check default true\n"
              << "option name SyzygyProbeLimit type spin default 7 min 0 max 7\n"
              // Chess960 declared for GUI compatibility — Hypersion plays
              // standard chess only, so we accept the option but castling
              // logic stays orthodox.
              << "option name UCI_Chess960 type check default false\n"
              << "option name Skill Level type spin default 20 min 0 max 20\n"
              << "option name UCI_LimitStrength type check default false\n"
              << "option name UCI_Elo type spin default 1500 min 500 max 3200\n"
              << "option name UCI_AnalyseMode type check default false\n"
              << "option name UCI_ShowWDL type check default false\n"
              << "option name Contempt type spin default 0 min -200 max 200\n"
              // Opponent-aware play: declaring UCI_Opponent makes lichess-bot
              // (via python-chess) send opponent info as `<title> <rating>
              // <player_type> <name>` after `ucinewgame`. UCI_MatchOpponent=true
              // tells Hypersion to extract the rating and auto-weaken to it.
              << "option name UCI_Opponent type string default <empty>\n"
              << "option name UCI_MatchOpponent type check default false\n"
              // Per-game rated flag (set by lichess-bot before each game).
              // When true AND UCI_MatchOpponent=true, opponent matching is
              // SUPPRESSED — engine plays at full strength because rated
              // games matter for ELO. When false (casual), opponent matching
              // applies normally.
              << "option name UCI_GameRated type check default false\n"
              << "uciok" << std::endl;
}

void cmd_isready()    { std::cout << "readyok" << std::endl; }

void cmd_ucinewgame() {
    Search::Threads.stop_all();
    Search::Threads.wait_all();
    Search::Threads.clear_all();
    NNUE::new_game();
    reset_position();
}

// "position [startpos | fen <fen>] [moves m1 m2 ...]"
void cmd_position(std::istringstream& is) {
    std::string token, fen;
    is >> token;

    if (token == "startpos") {
        fen = StartFEN;
        is >> token;
    } else if (token == "fen") {
        while (is >> token && token != "moves") fen += token + ' ';
    } else {
        return;
    }

    statePly = 0;
    pos.set(fen, &states[statePly++]);

    if (token == "moves") {
        while (is >> token) {
            Move m = parse_uci_move(token, pos);
            if (m == Move::none()) break;
            pos.do_move(m, states[statePly++]);
        }
    }
}

// "go ..." — supports depth / movetime / wtime / btime / winc / binc / movestogo / nodes / infinite / ponder / mate / perft
void cmd_go(std::istringstream& is) {
    SearchLimits lim;
    std::string token;
    while (is >> token) {
        if      (token == "depth")     is >> lim.depth;
        else if (token == "movetime")  is >> lim.movetime;
        else if (token == "wtime")     is >> lim.time[WHITE];
        else if (token == "btime")     is >> lim.time[BLACK];
        else if (token == "winc")      is >> lim.inc [WHITE];
        else if (token == "binc")      is >> lim.inc [BLACK];
        else if (token == "movestogo") is >> lim.movestogo;
        else if (token == "nodes")     is >> lim.nodes;
        else if (token == "infinite")  lim.infinite = true;
        else if (token == "ponder")    lim.ponder   = true;
        else if (token == "mate")      is >> lim.mate;
        else if (token == "searchmoves") {
            // Each remaining whitespace-separated token until end-of-line is
            // a UCI move string. Push valid ones onto lim.searchMoves; the
            // search will skip any root move not in the list.
            std::string mvs;
            while (is >> mvs) {
                Move m = parse_uci_move(mvs, pos);
                if (m != Move::none()) lim.searchMoves.push_back(m);
            }
            break;   // searchmoves consumes everything to end of line
        }
        else if (token == "perft")     {
            int d; is >> d;
            TimePoint t0 = now();
            perft_divide(pos, d);
            std::cout << "info string perft " << d << " took " << (now() - t0) << " ms" << std::endl;
            return;
        }
    }
    // Carry user-tunable strength settings into the search.
    lim.skillLevel    = Options.skillLevel;
    lim.limitStrength = Options.limitStrength;
    lim.uciElo        = Options.uciElo;
    lim.multiPv       = std::max(1, Options.multiPV);
    lim.moveOverhead  = Options.moveOverhead;
    lim.contempt      = Options.contempt;
    lim.showWDL       = Options.showWDL;

    // Try the opening book first — but never when in analyse mode (the GUI
    // wants the engine's actual evaluation, not a pre-canned book reply).
    if (Options.ownBook && !Options.analyseMode && Book::is_open()) {
        Move bm = Book::probe(pos, Options.bookBest);
        if (bm != Move::none()) {
            Square from = bm.from_sq(), to = bm.to_sq();
            std::string s;
            s += char('a' + file_of(from));
            s += char('1' + rank_of(from));
            s += char('a' + file_of(to));
            s += char('1' + rank_of(to));
            if (bm.type_of() == MT_PROMOTION) {
                constexpr char promoChar[] = " pnbrqk";
                s += promoChar[bm.promotion_type()];
            }
            std::cout << "info string book move " << s << '\n'
                      << "bestmove " << s << std::endl;
            return;
        }
    }

    Search::Threads.start(pos, lim);
}

void cmd_stop() { Search::Threads.stop_all(); }

// "setoption name <Name> [value <Value>]"
void cmd_setopt(std::istringstream& is) {
    std::string token, name, value;
    is >> token;   // expect "name"
    if (token != "name") return;
    while (is >> token && token != "value") {
        if (!name.empty()) name += ' ';
        name += token;
    }
    while (is >> token) {
        if (!value.empty()) value += ' ';
        value += token;
    }

    auto eq = [&](const char* s) {
        std::string a = name, b = s;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        return a == b;
    };

    auto parse_int = [&](int& out) {
        char* endp = nullptr;
        long v = std::strtol(value.c_str(), &endp, 10);
        if (endp != value.c_str()) out = int(v);
    };
    auto parse_bool = [&](bool& out) {
        std::string v = value;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        out = (v == "true" || v == "1" || v == "yes" || v == "on");
    };

    if      (eq("Hash"))           { parse_int(Options.hashMB);       apply_hash(); }
    else if (eq("Threads"))        { parse_int(Options.threads);
                                     Search::Threads.set_size(std::max(1, Options.threads)); }
    else if (eq("MultiPV"))        { parse_int(Options.multiPV); }
    else if (eq("Move Overhead"))  { parse_int(Options.moveOverhead); }
    else if (eq("Clear Hash"))     { TT.clear(); }
    else if (eq("Ponder"))         { /* accepted, no behavior yet */ }
    else if (eq("OwnBook"))        { parse_bool(Options.ownBook); }
    else if (eq("BookBestMove"))   { parse_bool(Options.bookBest); }
    else if (eq("BookFile"))       {
        Options.bookFile = value;
        Book::close();
        if (Options.ownBook && !value.empty() && value != "<empty>")
            Book::open(value);
    }
    else if (eq("EvalFile"))       {
        Options.evalFile = value;
        if (!value.empty() && value != "<empty>") NNUE::load_big(value);
    }
    else if (eq("EvalFileSmall"))  {
        Options.evalFileSmall = value;
        if (!value.empty() && value != "<empty>") NNUE::load_small(value);
    }
    else if (eq("SyzygyPath"))     {
        Options.syzygyPath = value;
        Syzygy::init(value);
    }
    else if (eq("SyzygyProbeDepth")) {
        int d = 1; std::istringstream(value) >> d;
        Syzygy::set_probe_depth(std::clamp(d, 1, 100));
    }
    else if (eq("Syzygy50MoveRule")) {
        bool b = true; parse_bool(b);
        Syzygy::set_50_move_rule(b);
    }
    else if (eq("SyzygyProbeLimit")) {
        int n = 7; std::istringstream(value) >> n;
        Syzygy::set_probe_limit(std::clamp(n, 0, 7));
    }
    else if (eq("UCI_Chess960")) {
        // Accepted for compatibility; Hypersion plays standard rules only.
        // No action — silently absorb the option so GUIs don't error out.
    }
    else if (eq("Skill Level"))    { parse_int(Options.skillLevel);
                                     Options.skillLevel = std::clamp(Options.skillLevel, 0, 20); }
    else if (eq("UCI_LimitStrength")) parse_bool(Options.limitStrength);
    else if (eq("UCI_Elo"))        { parse_int(Options.uciElo);
                                     Options.uciElo = std::clamp(Options.uciElo, 500, 3200); }
    else if (eq("UCI_AnalyseMode")) parse_bool(Options.analyseMode);
    else if (eq("UCI_ShowWDL"))     parse_bool(Options.showWDL);
    else if (eq("Contempt"))       { parse_int(Options.contempt);
                                     Options.contempt = std::clamp(Options.contempt, -200, 200); }
    else if (eq("UCI_MatchOpponent")) parse_bool(Options.matchOpponent);
    else if (eq("UCI_GameRated"))     parse_bool(Options.gameRated);
    else if (eq("UCI_Opponent"))   {
        // python-chess sends: "<title> <rating> <player_type> <name>"
        // (e.g. "none 600 human RisotPlayer" or "GM 2700 computer Stockfish").
        // We scan tokens for:
        //   - the first integer in [100, 4000]  -> opponent rating
        //   - "computer"/"engine" or "human"    -> player type
        // and apply the ELO limit ONLY for humans. Bots always get full strength.
        if (Options.matchOpponent) {
            // Reset to full strength first so each new game starts clean.
            // (e.g. previous game was vs human at low ELO -> next game vs bot
            // should not inherit the limit.)
            Options.limitStrength = false;

            std::istringstream iss(value);
            std::string tok;
            int rating = -1;
            bool isHuman = true;            // assume human if not otherwise indicated
            bool typeSeen = false;
            while (iss >> tok) {
                // Type token detection (case-insensitive).
                std::string lower = tok;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower == "computer" || lower == "engine" || lower == "bot") {
                    isHuman = false; typeSeen = true;
                } else if (lower == "human") {
                    isHuman = true;  typeSeen = true;
                } else if (rating < 0) {
                    // First integer in valid range = rating.
                    char* endp = nullptr;
                    long n = std::strtol(tok.c_str(), &endp, 10);
                    if (endp && *endp == '\0' && n >= 100 && n <= 4000)
                        rating = int(n);
                }
            }
            // Rated game override: regardless of opponent type, play full
            // strength because rated games count for ELO. Only casual
            // games trigger the educational ELO matching for humans.
            if (Options.gameRated) {
                std::cerr << "info string UCI_MatchOpponent: rated game"
                          << (rating > 0 ? (" (opp=" + std::to_string(rating) + ")") : "")
                          << " -> full strength\n";
            } else if (!isHuman) {
                std::cerr << "info string UCI_MatchOpponent: opp is computer/bot"
                          << (rating > 0 ? (" (rating=" + std::to_string(rating) + ")") : "")
                          << " -> full strength\n";
            } else if (rating > 0) {
                int offset = opponent_match_offset(rating);
                int target = std::clamp(rating + offset, Options.matchFloor, Options.matchCeiling);
                Options.limitStrength = true;
                Options.uciElo = target;
                std::cerr << "info string UCI_MatchOpponent: casual human opp=" << rating
                          << " offset=+" << offset
                          << " -> UCI_Elo=" << target << '\n';
            } else {
                std::cerr << "info string UCI_MatchOpponent: no rating found"
                          << (typeSeen ? " (human)" : "")
                          << " -> full strength\n";
            }
        }
    }
}

void cmd_perft(std::istringstream& is) {
    int depth = 1; is >> depth;
    TimePoint t0 = now();
    perft_divide(pos, depth);
    std::cout << "info string perft " << depth << " took " << (now() - t0) << " ms" << std::endl;
}

void cmd_eval() {
    Value v = Eval::evaluate(pos);
    Color stm = pos.side_to_move();
    Value vWhite = stm == WHITE ? v : -v;     // standardise to white-POV for display
    int phase = 0;
    constexpr int PhV[7] = { 0, 0, 1, 1, 2, 4, 0 };
    for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
        phase += int(popcount(pos.pieces(pt))) * PhV[pt];
    if (phase > 24) phase = 24;
    // Display in SF "1 pawn = 100 cp" convention (divide internal eval by 5).
    // The `eval` command is a human-facing debug tool, so consistent units
    // with `score cp` in info lines and external GUI eval bars matter.
    constexpr int OUTPUT_CP_DIVISOR = 5;
    std::cout << pos.pretty()
              << "\n----- Eval (white POV) -----\n"
              << "  side to move      : " << (stm == WHITE ? "white" : "black") << '\n'
              << "  static eval (cp)  : " << (int(vWhite) / OUTPUT_CP_DIVISOR) << '\n'
              << "  STM-relative eval : " << (int(v) / OUTPUT_CP_DIVISOR) << '\n'
              << "  game phase 0..24  : " << phase
              << "  ("
              << (phase >= 18 ? "midgame" : phase <= 6 ? "endgame" : "tapered")
              << ")\n"
              << "  pieces material   : W=" << pos.non_pawn_material(WHITE)
                                     << "  B=" << pos.non_pawn_material(BLACK) << '\n'
              << "  pawn-key bucket   : 0x" << std::hex << pos.pawn_key() << std::dec << '\n'
              << "----------------------------" << std::endl;
}

void cmd_d() { std::cout << pos.pretty() << std::endl; }

// Fixed benchmark workload — used by PGO and for tracking nps regressions.
// Searches a basket of openings/middlegames/endgames at a fixed depth and prints
// the cumulative node count and elapsed time.
void cmd_bench(std::istringstream& is) {
    int depth = 13;
    is >> depth;
    static const char* BenchFENs[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "8/8/8/8/3K4/8/3kp3/8 w - - 0 1",
        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
    };

    Search::Threads.stop_all(); Search::Threads.wait_all();
    Search::Threads.clear_all();

    TimePoint t0 = now();
    std::uint64_t totalNodes = 0;
    int posIdx = 0;
    for (const char* fen : BenchFENs) {
        statePly = 0;
        pos.set(fen, &states[statePly++]);
        SearchLimits lim;
        lim.depth = depth;
        Search::Threads.clear_all();
        Search::Threads.start(pos, lim);
        Search::Threads.wait_all();
        std::uint64_t posN = Search::Threads.total_nodes();
        std::cout << "\ninfo string position " << ++posIdx
                  << " nodes " << posN << std::endl;
        totalNodes += posN;
    }
    int64_t elapsed = std::max<int64_t>(1, now() - t0);
    std::uint64_t nps = (totalNodes * 1000) / std::uint64_t(elapsed);
    // OpenBench-compatible final line: tools that scrape `bench` output look
    // for a "Nodes searched: N" or similar signature on the LAST line. Print
    // both the human-readable summary AND a one-line signature for tools.
    std::cout << "\n==========================="
              << "\nTotal time : " << elapsed << " ms"
              << "\nNodes      : " << totalNodes
              << "\nNodes/sec  : " << nps
              << "\n===========================\n"
              << "info string Nodes searched: " << totalNodes << '\n'
              << "Nodes searched : " << totalNodes
              << std::endl;
}

}  // namespace

void loop(int argc, char** argv) {
    std::cout << engine_id() << " by " << ENGINE_AUTHOR << std::endl;
    // Surface compiled SIMD level so users / tournament managers can see
    // which build is running. Helps debugging "why is bench different on
    // your machine" reports.
#if defined(__AVX512F__)
    std::cerr << "info string compiled with AVX-512" << std::endl;
#elif defined(__AVXVNNI__) || defined(__AVX512VNNI__)
    std::cerr << "info string compiled with AVX-VNNI" << std::endl;
#elif defined(__AVX2__)
    std::cerr << "info string compiled with AVX2" << std::endl;
#elif defined(__SSE2__)
    std::cerr << "info string compiled with SSE2" << std::endl;
#else
    std::cerr << "info string compiled scalar" << std::endl;
#endif
    apply_hash();
    reset_position();

    // Auto-load opening book if the default file is present alongside the
    // binary. Failure is non-fatal — engine just runs without it.
    if (!Options.bookFile.empty() && Options.bookFile != "<empty>")
        Book::open(Options.bookFile);   // tries CWD; user can override via setoption

    // Auto-load SF18 NNUE networks from the binary's directory if present.
    // Quiet failure — classical eval kicks in if either or both are missing.
    if (!Options.evalFile.empty()      && Options.evalFile      != "<empty>")
        NNUE::load_big  (Options.evalFile);
    if (!Options.evalFileSmall.empty() && Options.evalFileSmall != "<empty>")
        NNUE::load_small(Options.evalFileSmall);

    std::string cli;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) cli += ' ';
        cli += argv[i];
    }

    std::string line;
    bool haveCliCmd = !cli.empty();

    // Bench-from-CLI shortcut: `Hypersion.exe bench [depth]` runs the workload
    // and exits without entering the UCI loop — used by OpenBench / PGO / scripts.
    if (haveCliCmd) {
        std::istringstream first(cli);
        std::string firstTok; first >> firstTok;
        if (firstTok == "bench") {
            cmd_bench(first);
            Search::Threads.wait_all();
            return;
        }
    }

    for (;;) {
        if (haveCliCmd) { line = cli; haveCliCmd = false; }
        else if (!std::getline(std::cin, line)) {
            // stdin closed — let any in-flight search finish so its output isn't truncated.
            // (Use `quit` if you want to force-abort an `infinite` search.)
            Search::Threads.wait_all();
            break;
        }

        std::istringstream is(line);
        std::string token;
        is >> token;

        if      (token == "uci")        cmd_uci();
        else if (token == "isready")    cmd_isready();
        else if (token == "ucinewgame") cmd_ucinewgame();
        else if (token == "position")   cmd_position(is);
        else if (token == "go")         cmd_go(is);
        else if (token == "stop")       cmd_stop();
        else if (token == "ponderhit")  {
            Search::Threads.reset_clock_start();   // budget starts NOW, not at go-ponder
            Search::Threads.ponder_flag().store(false);
        }
        else if (token == "perft")      cmd_perft(is);
        else if (token == "perftsuite") perft_run_suite();
        else if (token == "eval")       cmd_eval();
        else if (token == "d")          cmd_d();
        else if (token == "setoption")  cmd_setopt(is);
        else if (token == "quit" || token == "exit") {
            cmd_stop();
            Search::Threads.wait_all();
            break;
        }
        else if (token == "bench") {
            cmd_bench(is);
        }
        else if (!token.empty()) {
            // Ignore unknown tokens silently, per UCI spec.
        }
    }
}

}  // namespace hypersion::UCI
