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
    bool ponder       = false;       // UCI Ponder mode (think on opponent's clock).
                                     // Used by TimeManager to give an extra 25 % to the
                                     // optimum-time budget — the engine spends part of its
                                     // think on the opponent's time, so the wall-clock
                                     // optimum can safely be larger.
    std::string bookFile = "Perfect2023.bin";
    std::string evalFile      = "nn-c288c895ea92.nnue";   // SF18 big default
    std::string evalFileSmall = "nn-37f18f62d772.nnue";   // SF18 small default
    // 2026-05-17 audit uci #22: previously the EvalUseSmallOnly UCI option
    // had no backing field in Options — `setoption name EvalUseSmallOnly
    // value true` wrote only to nnue.cpp's namespace-static g_small_only_flag.
    // Now mirrored here so all UCI options live in one struct and tools that
    // inspect Options state see consistent values.
    bool evalUseSmallOnly = false;
    std::string syzygyPath = "<empty>";
    // Opponent-aware strength matching. When matchOpponent=true and the GUI
    // sends UCI_Opponent (lichess-bot does this automatically), Hypersion
    // plays at a level roughly BELOW the opponent across all rating bands.
    // This gives the user the satisfying experience of beating the bot
    // while still being challenged.
    //
    // Offset curve (small bump applied at <1000 — bot was too weak there):
    //     <800       -125  (was -150)
    //     800-999    -100  (was -125)
    //     1000-1199  -125
    //     1200-1599  -100
    //     1600-1999   -75
    //     2000-2399   -50
    //     2400-2599   -25
    //     >=2600        0  (master+: exact match)
    //
    // Final target clamped to [matchFloor=500, matchCeiling=3300].
    bool matchOpponent = false;
    int  matchFloor    = 500;
    int  matchCeiling  = 3300;
    // Per-game rated flag, set by lichess-bot before each game. When the
    // game is rated, we override matchOpponent and play at full strength
    // regardless of opponent rating — rated games count for ELO and
    // shouldn't be deliberately weakened.
    bool gameRated     = false;
    // Per-game tournament flag, set by lichess-bot before each tournament
    // game. When true, opponent-ELO matching is applied EVEN if the game
    // is rated — tournament play prioritizes balanced match-ups for the
    // user's tournament-organizing intent over preserving global ELO.
    // Empty / false (default) = previous behavior.
    bool gameTournament = false;
    // ── Lc0-inspired persistent correction-history ─────────────────────────
    // PersistCorrHist (default OFF as of 2026-05-17): on engine startup,
    // load corr-history from CorrHistFile if present; on ucinewgame (before
    // per-game decay), save to that file. Lets the engine remember which
    // pawn structures had systematically-wrong static evals from previous
    // games — a form of "learn from its mistakes" without needing offline
    // NNUE retrain.
    //
    // 2026-05-17 changed default to FALSE because the auto-load at startup
    // imported accumulated history that varied across runs, producing
    // non-deterministic behavior in endgame conversion (KBBK / KPK / KBNK
    // would sometimes mate in 20 moves, sometimes shuffle to a 50-move-rule
    // draw on the same FEN). Validation SPRT at 400g showed only ~+2.6 ELO
    // sub-noise benefit from the feature anyway. Users who want online
    // learning can opt in via `setoption name PersistCorrHist value true`.
    bool        persistCorrHist = false;
    std::string corrHistFile    = "hypersion_corrhist.bin";
} Options;

// Opponent matching offset. The bot plays consistently BELOW the
// opponent across all rating bands, narrowing toward a small (-50)
// underbid at master level. Goal: opponent should win a meaningful
// share of games.
//
// 2026-05-09: shifted entire curve -50 ELO ("just a little weaker"
// per user feedback). Previous curve was -125/-100/-125/-100/-75/
// -50/-25/0 across the bands; this curve is uniformly 50 lower.
// Effect: against a 1600 opponent the bot now targets 1450 (was
// 1500); against a 2200 opp targets 2100 (was 2150); against 2700+
// targets 2650 (was 2700). 50 ELO is ~1 game in 4 worth of win-rate
// concession — noticeable but not crushing.
inline int opponent_match_offset(int rating) {
    if (rating <  800) return -175;
    if (rating < 1000) return -150;
    if (rating < 1200) return -175;
    if (rating < 1600) return -150;
    if (rating < 2000) return -125;
    if (rating < 2400) return -100;
    if (rating < 2600) return  -75;
    return                      -50;   // master+: small underbid
}

void apply_hash() { TT.resize(std::max(1, Options.hashMB)); }

void reset_position() { statePly = 0; pos.set(StartFEN, &states[statePly++]); }

// Counts own-search moves per game (not book hits). Reset to 0 in
// cmd_ucinewgame. Incremented just before Search::Threads.start() runs.
// TimeManager boosts optimum-time when value is in [1..3] — the first
// 3 searches have no TT continuity. See testing/BLUNDER_ANALYSIS.md
// (2026-05-15).
int g_ownSearchesThisGame = 0;

Move parse_uci_move(const std::string& input, const Position& p) {
    if (input.length() < 4) return Move::none();
    // 2026-05-17 finding #37: SF UCIEngine::to_move lowercases the input
    // before comparing (SF18 uci.cpp:605). Mixed-case input like "E2E4"
    // is valid per UCI spec; Hypersion previously rejected it.
    std::string str = input;
    for (char& c : str) c = char(std::tolower(static_cast<unsigned char>(c)));
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

        // Chess960 castling: GUIs (and python-chess with chess960=True) often
        // send castling moves in king-takes-rook notation, e.g. "e1h1" when
        // the king is on E1 and the kingside rook on H1. Hypersion stores
        // the move with king-to-G1/C1 (standard king destination), so we
        // also match against the king-takes-rook representation.
        if (m.type_of() == MT_CASTLING) {
            bool kingSide = file_of(to) == FILE_G;
            CastlingRights cr = (p.side_to_move() == WHITE)
                ? (kingSide ? WHITE_OO : WHITE_OOO)
                : (kingSide ? BLACK_OO : BLACK_OOO);
            Square rfrom = p.castling_rook_square(cr);
            std::string ktr;
            ktr += char('a' + file_of(from));
            ktr += char('1' + rank_of(from));
            ktr += char('a' + file_of(rfrom));
            ktr += char('1' + rank_of(rfrom));
            if (ktr == str) return m;
        }
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
              // Phase 2.5 experiment toggle: force small-net for all positions.
              // Trades eval accuracy in normal positions for ~3-5x NPS (cache
              // friendly). Test SPRT vs default before flipping permanently.
              << "option name EvalUseSmallOnly type check default false\n"
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
              << "option name UCI_Elo type spin default 1500 min 500 max 3300\n"
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
              // Per-game tournament flag (set by lichess-bot before each
              // tournament game). When true, opponent-ELO matching applies
              // even if UCI_GameRated=true. Lets the user run rated
              // tournaments with rating-balanced play.
              << "option name UCI_GameTournament type check default false\n"
              // Lc0-inspired online "learn from mistakes" toggle. When true,
              // the corr-history table is loaded from CorrHistFile on startup
              // and saved on every ucinewgame, so the engine remembers
              // eval corrections across games / sessions. See history.h.
              << "option name PersistCorrHist type check default false\n"
              << "option name CorrHistFile type string default hypersion_corrhist.bin\n"
              << "uciok" << std::endl;
}

void cmd_isready()    { std::cout << "readyok" << std::endl; }

void cmd_ucinewgame() {
    Search::Threads.stop_all();
    Search::Threads.wait_all();
    // Lc0-inspired persistent corr-history: save BEFORE clear_all() so the
    // just-finished game's learning is preserved across `ucinewgame`. The
    // save is silent on failure (file lock, full disk) -- engine still
    // works fine, just doesn't accumulate cross-game knowledge.
    if (Options.persistCorrHist && !Options.corrHistFile.empty()
        && Options.corrHistFile != "<empty>") {
        Search::Threads.save_corr_hist(Options.corrHistFile);
    }
    Search::Threads.clear_all();
    NNUE::new_game();
    reset_position();
    g_ownSearchesThisGame = 0;  // first own search will get index 1 → boost
    // After clear_all() wiped the tables, re-load them so the new game
    // starts with the cross-game learned baseline. (clear_all() is needed
    // for transposition table + non-corr histories which DO want fresh.)
    if (Options.persistCorrHist && !Options.corrHistFile.empty()
        && Options.corrHistFile != "<empty>") {
        Search::Threads.load_corr_hist(Options.corrHistFile);
    }
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
    // 2026-05-17 audit uci #45: capture wall-clock at cmd_go entry so book
    // probe + ThreadPool::start() latency is counted against the move
    // budget (SF18 uci.cpp:204 does the same).
    lim.goStartTime = now();
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
    lim.analyseMode   = Options.analyseMode;
    lim.ponderEnabled = Options.ponder;

    // Try the opening book first — but never when in analyse mode (the GUI
    // wants the engine's actual evaluation, not a pre-canned book reply).
    if (Options.ownBook && !Options.analyseMode && Book::is_open()) {
        Move bm = Book::probe(pos, Options.bookBest);
        if (bm != Move::none()) {
            // Book hit: don't increment own-search counter (we didn't search).
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

    // Book missed (or skipped). About to run search — bump counter and
    // pass the 1-indexed search count to TimeManager.
    lim.ownSearchIndex = ++g_ownSearchesThisGame;

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
    else if (eq("Clear Hash"))     {
        // 2026-05-17 finding #19: SF18's `Clear Hash` calls
        // `search_clear()` (engine.cpp:99-102) which clears BOTH the TT
        // and per-thread histories. Hypersion previously cleared only the
        // TT, leaving stale corrhist / butterfly / contHist data that
        // would bias subsequent searches with information from the old
        // game. Now matches SF behaviour.
        Search::Threads.wait_all();
        TT.clear();
        Search::Threads.clear_all();
    }
    else if (eq("Ponder"))         { parse_bool(Options.ponder); }
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
        if (value.empty() || value == "<empty>") {
            NNUE::unload();   // forces classical fallback for testing
        } else {
            NNUE::load_big(value);
        }
    }
    else if (eq("EvalFileSmall"))  {
        Options.evalFileSmall = value;
        if (value.empty() || value == "<empty>") {
            // small net handled by unified unload()
        } else {
            NNUE::load_small(value);
        }
    }
    else if (eq("EvalUseSmallOnly")) {
        parse_bool(Options.evalUseSmallOnly);
        NNUE::set_small_only(Options.evalUseSmallOnly);
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
                                     // 2026-05-17 finding #9: declared max is 3300, was clamping to 3200.
                                     Options.uciElo = std::clamp(Options.uciElo, 500, 3300); }
    else if (eq("UCI_AnalyseMode")) parse_bool(Options.analyseMode);
    else if (eq("UCI_ShowWDL"))     parse_bool(Options.showWDL);
    else if (eq("Contempt"))       { parse_int(Options.contempt);
                                     Options.contempt = std::clamp(Options.contempt, -200, 200); }
    else if (eq("UCI_MatchOpponent"))   parse_bool(Options.matchOpponent);
    else if (eq("UCI_GameRated"))       parse_bool(Options.gameRated);
    else if (eq("UCI_GameTournament"))  parse_bool(Options.gameTournament);
    else if (eq("PersistCorrHist")) {
        bool old_val = Options.persistCorrHist;
        parse_bool(Options.persistCorrHist);
        // 2026-05-17 determinism fix: when persistence is turned OFF, also
        // wipe the in-memory corr-history tables. The engine loads
        // hypersion_corrhist.bin at startup (before any setoption arrives),
        // so disabling persistence later would leave the stale loaded data
        // biasing search — a non-deterministic state across runs.
        // Now `setoption name PersistCorrHist value false` produces a clean
        // search state, matching what users expect for SPRT / debug runs.
        if (old_val && !Options.persistCorrHist) {
            Search::Threads.wait_all();
            Search::Threads.clear_all();
        }
    }
    else if (eq("CorrHistFile"))        Options.corrHistFile = value;
    else if (eq("UCI_Opponent"))   {
        // python-chess sends: "<title> <rating> <player_type> <name>"
        // (e.g. "none 600 human RisotPlayer" or "GM 2700 computer Stockfish").
        // We scan tokens for:
        //   - the first integer in [100, 4000]  -> opponent rating
        //   - "computer"/"engine" or "human"    -> player type (informational)
        // and apply the ELO limit in CASUAL games regardless of human/bot.
        // Rated games always full-strength (counts for ELO).
        if (Options.matchOpponent) {
            // Reset to full strength first so each new game starts clean.
            // (e.g. previous game was vs low-rated opp -> next vs higher
            // shouldn't inherit the limit.)
            Options.limitStrength = false;

            std::istringstream iss(value);
            std::string tok;
            int rating = -1;
            bool isHuman = true;            // informational only — kept for log clarity
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
            // strength because rated games count for ELO.
            // Casual games trigger ELO matching for ANY opponent (human OR
            // bot) when a rating is provided. This keeps casual games
            // educational/balanced even against weaker bots.
            // Tournament override: when UCI_GameTournament=true, ELO
            // matching applies even in rated games — user explicitly opted
            // into tournament-style balanced play.
            const char* oppType = typeSeen ? (isHuman ? "human" : "bot") : "unknown";
            const bool ratedOverride = Options.gameRated && !Options.gameTournament;
            if (ratedOverride) {
                std::cerr << "info string UCI_MatchOpponent: rated game"
                          << (rating > 0 ? (" (opp=" + std::to_string(rating) + ")") : "")
                          << " -> full strength\n";
            } else if (rating > 0) {
                int offset = opponent_match_offset(rating);
                int target = std::clamp(rating + offset, Options.matchFloor, Options.matchCeiling);
                Options.limitStrength = true;
                Options.uciElo = target;
                const char* gameType = Options.gameTournament
                    ? (Options.gameRated ? "rated tournament" : "casual tournament")
                    : "casual";
                std::cerr << "info string UCI_MatchOpponent: " << gameType << ' ' << oppType
                          << " opp=" << rating
                          << " offset=+" << offset
                          << " -> UCI_Elo=" << target << '\n';
            } else {
                std::cerr << "info string UCI_MatchOpponent: no rating found"
                          << " (" << oppType << ")"
                          << " -> full strength\n";
            }
        }
    }
    // SPSA-tunable knob — `setoption name Tune_<KNOB> value <int>` writes
    // to a runtime variable in src/search.cpp (search tunables) or
    // src/evaluate.cpp (classical-eval tunables). Used by SPSA scripts to
    // perturb constants between matches without rebuilding. Names match
    // the C++ identifiers (e.g. RFP_MARGIN_PER_DEPTH, PassedRank4, ...).
    else if (name.rfind("Tune_", 0) == 0) {
        int v = 0; std::istringstream(value) >> v;
        std::string knob = name.substr(5);
        if (Search::set_tunable(knob, v) || Eval::set_tunable(knob, v)) {
            std::cerr << "info string Tune_" << knob << " = " << v << '\n';
        } else {
            std::cerr << "info string Tune_: unknown knob '" << knob << "'\n";
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
    // Allow `--no-nnue-default` on the CLI to skip the auto-load of NNUE
    // networks at startup. Used by classical-only benchmarking + SPSA
    // campaigns over eval_params (e.g. testing/spsa_eval.py) to avoid
    // the ~1.5 s NNUE load on each engine spawn followed by an immediate
    // unload via setoption EvalFile=<empty>.
    bool noNnueDefault = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-nnue-default") {
            noNnueDefault = true;
            Options.evalFile      = "<empty>";
            Options.evalFileSmall = "<empty>";
        }
    }
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
    // Skipped entirely when --no-nnue-default was passed (see top of loop).
    if (!noNnueDefault) {
        if (!Options.evalFile.empty()      && Options.evalFile      != "<empty>")
            NNUE::load_big  (Options.evalFile);
        if (!Options.evalFileSmall.empty() && Options.evalFileSmall != "<empty>")
            NNUE::load_small(Options.evalFileSmall);
        // 2026-05-17 audit uci #48: SF18 calls verify_networks() before
        // entering search; Hypersion silently falls back to classical eval.
        // Emit a single info string at startup so misconfigured EvalFile /
        // missing .nnue alongside the exe surfaces immediately instead of
        // mystifying users with weak play.
        if (!NNUE::is_loaded())
            std::cerr << "info string warning: no NNUE network loaded — "
                         "using classical eval (check EvalFile / EvalFileSmall paths)"
                      << std::endl;
    }

    // Lc0-inspired persistent corr-history load. If the file exists from
    // a previous session, populate all worker threads with the saved
    // tables. Failure is silent — engine just starts with empty histories
    // (the normal first-run state).
    if (Options.persistCorrHist && !Options.corrHistFile.empty()
        && Options.corrHistFile != "<empty>") {
        if (Search::Threads.load_corr_hist(Options.corrHistFile)) {
            std::cerr << "info string loaded corr-history from "
                      << Options.corrHistFile << '\n';
        }
    }

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
            // Final save of corr-history on clean shutdown so the last
            // game's learning isn't lost when the engine exits.
            if (Options.persistCorrHist && !Options.corrHistFile.empty()
                && Options.corrHistFile != "<empty>") {
                Search::Threads.save_corr_hist(Options.corrHistFile);
            }
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
