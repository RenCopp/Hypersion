// Hypersion — entry point.
// All initialization happens here, then control passes to the UCI loop.

#include "bitboard.h"
#include "evaluate.h"
#include "misc.h"
#include "nnue.h"
#include "position.h"
#include "search.h"
#include "uci.h"
#include "zobrist.h"

int main(int argc, char** argv) {
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
