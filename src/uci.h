// Hypersion — UCI protocol loop.
// Reads commands from stdin line-by-line, dispatches to handlers, writes to stdout.

#ifndef HYPERSION_UCI_H
#define HYPERSION_UCI_H

#include <string>

namespace hypersion::UCI {

// Enter the UCI loop. Returns when `quit` is received (or stdin closed).
// Accepts an optional CLI command (e.g. `bench`, `perft 5`) run once before the loop.
void loop(int argc, char** argv);

}  // namespace hypersion::UCI

#endif  // HYPERSION_UCI_H
