// Hypersion — entry point.
// All initialization happens here, then control passes to the UCI loop.

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
    // Auto-flush stdout after every operation. Without this, when stdout is
    // a pipe (e.g. lichess-bot, cutechess, python-chess), C++ default fully-
    // buffered mode can leave info/bestmove output stuck in the buffer until
    // a flush, causing GUIs to wait or interact with stale state. SF and
    // most strong engines do this. (Also fixes a hard-to-reproduce crash on
    // certain Q+Q positions when run via piped stdio.)
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
