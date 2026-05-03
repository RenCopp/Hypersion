// Hypersion — entry point.
// All initialization happens here, then control passes to the UCI loop.

#include <cstdio>
#include <iostream>

#include "bitboard.h"
#include "evaluate.h"
#include "misc.h"
#include "nnue.h"
#include "position.h"
#include "search.h"
#include "uci.h"
#include "zobrist.h"

int main(int argc, char** argv) {
    // Disable I/O buffering at both C++ and C levels. Without this, when
    // stdout is a Win32 anonymous pipe (lichess-bot, cutechess, python-chess
    // subprocess.PIPE), buffered output can deadlock with the GUI's reader
    // and Hypersion's search thread can crash with ACCESS_VIOLATION on
    // certain dense-attack positions. Most strong engines (Stockfish, etc.)
    // disable buffering for this exact reason.
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    std::ios::sync_with_stdio(false);   // decouple C++ streams from C stdio
    std::cin.tie(nullptr);              // don't flush cout before each cin read
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // C-level: completely unbuffered
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    hypersion::Bitboards::init();
    hypersion::Zobrist::init();
    hypersion::Position::init();
    hypersion::Eval::init();
    hypersion::NNUE::init();      // build threat-feature lookup tables
    hypersion::Search::init();

    hypersion::UCI::loop(argc, argv);

    hypersion::Search::shutdown();
    return 0;
}
