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
    // Auto-flush stdout after every C++ stream operation (Stockfish-style).
    // Without this, when stdout is a pipe (lichess-bot, cutechess), C++
    // default fully-buffered mode can leave info/bestmove output stuck.
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

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
