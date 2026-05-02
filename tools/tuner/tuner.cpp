// Hypersion — Texel-style classical-eval tuner.
//
// Loads a labeled position file (FEN | game-result) and reports the mean
// squared error between Hypersion's classical eval (passed through a
// sigmoid) and the labels. With --tune, does a single coordinate-descent
// pass over the listed parameters and prints suggested adjustments.
//
// The scoring loss is the standard Texel one:
//
//     loss = mean( (sigmoid(K * eval / 400) - result) ^ 2 )
//
// where eval is in centipawns (white-POV), result is 0/0.5/1, and K is a
// tuning constant (~1.0 for engines whose eval is roughly in pawn units).
//
// Format of the position file (one record per line):
//     <fen> | <result>
// where <result> is "1" (white won), "0" (black won), or "0.5" (drew).
//
// Build:
//     make tuner            (after adding a target to the top-level Makefile)
// Use:
//     tuner --in positions.txt
//     tuner --in positions.txt --tune
//
// Status: minimum viable implementation. The position file format is
// trivial — extracting it from PGN (parsing SAN move text) is a separate
// tool that needs to be rebuilt; see tools/tuner/README.md.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/bitboard.h"
#include "../../src/eval_params.h"
#include "../../src/evaluate.h"
#include "../../src/position.h"
#include "../../src/zobrist.h"

using namespace hypersion;

namespace {

double sigmoid(double x, double k) {
    return 1.0 / (1.0 + std::exp(-k * x / 400.0));
}

struct LabeledPos {
    std::string fen;
    double      result;          // 0.0, 0.5, or 1.0
};

std::vector<LabeledPos> load_positions(const std::string& path) {
    std::vector<LabeledPos> records;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return records;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto bar = line.rfind('|');
        if (bar == std::string::npos) continue;
        std::string fen = line.substr(0, bar);
        // strip trailing space
        while (!fen.empty() && fen.back() == ' ') fen.pop_back();
        std::string r = line.substr(bar + 1);
        // strip leading space
        size_t i = 0;
        while (i < r.size() && r[i] == ' ') ++i;
        r = r.substr(i);
        double result = std::strtod(r.c_str(), nullptr);
        if (result < 0.0 || result > 1.0) continue;
        records.push_back({fen, result});
    }
    return records;
}

// Mean squared error between (sigmoid of eval) and result, over the dataset.
double mse(const std::vector<LabeledPos>& records, double k) {
    Position pos;
    StateInfo state;
    double sum = 0.0;
    long long n = 0;
    for (const auto& rec : records) {
        pos.set(rec.fen, &state);
        Value v = Eval::evaluate(pos);
        // Convert STM-relative cp to white-POV cp for sigmoid.
        double cp = (pos.side_to_move() == WHITE) ? double(v) : -double(v);
        double p = sigmoid(cp, k);
        double d = p - rec.result;
        sum += d * d;
        ++n;
    }
    return n > 0 ? sum / double(n) : 0.0;
}

// Find the K that minimises MSE on the dataset (just-in-case calibration).
double calibrate_k(const std::vector<LabeledPos>& records) {
    double best_k   = 1.0;
    double best_mse = 1e9;
    for (double k = 0.4; k <= 2.0; k += 0.05) {
        double m = mse(records, k);
        if (m < best_mse) {
            best_mse = m;
            best_k   = k;
        }
    }
    return best_k;
}

}  // namespace

int main(int argc, char** argv) {
    Bitboards::init();
    Zobrist::init();
    Position::init();
    Eval::init();

    std::string in_path;
    bool        tune = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--in")   in_path = argv[++i];
        else if (a == "--tune") tune    = true;
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                "Usage: tuner --in positions.txt [--tune]\n"
                "  Each line: <fen> | <result>  where result is 0, 0.5, or 1\n");
            return 0;
        }
    }
    if (in_path.empty()) {
        std::fprintf(stderr, "--in <file> is required\n");
        return 1;
    }

    auto records = load_positions(in_path);
    std::printf("loaded %zu labeled positions from %s\n",
                records.size(), in_path.c_str());
    if (records.empty()) return 1;

    double k = calibrate_k(records);
    double m = mse(records, k);
    std::printf("baseline: K=%.3f  MSE=%.6f\n", k, m);
    std::printf("(lower MSE = eval matches results better)\n");

    if (!tune) return 0;

    // ----- Coordinate-descent tune loop. ------------------------------------
    // Walks every parameter in `Eval::params()`, tries +/-1 (then +/-2, +/-4
    // step sizes), and keeps the change if MSE drops. Repeats until a full
    // sweep produces no improvement.
    auto& p = Eval::params();
    struct Knob { const char* name; int* ptr; int floor; int ceil; };
    Knob knobs[] = {
        {"IsolatedPawnPenalty",   &p.IsolatedPawnPenalty,   0,  60},
        {"DoubledPawnPenalty",    &p.DoubledPawnPenalty,    0,  60},
        {"BackwardPawnPenalty",   &p.BackwardPawnPenalty,   0,  40},
        {"BishopPairBonusMG",     &p.BishopPairBonusMG,    -10, 80},
        {"BishopPairBonusEG",     &p.BishopPairBonusEG,    -10, 100},
        {"RookOpenFileMG",        &p.RookOpenFileMG,        0,  60},
        {"RookOpenFileEG",        &p.RookOpenFileEG,        0,  60},
        {"RookSemiOpenFileMG",    &p.RookSemiOpenFileMG,    0,  40},
        {"RookSemiOpenFileEG",    &p.RookSemiOpenFileEG,    0,  40},
        {"KnightOutpostMG",       &p.KnightOutpostMG,       0,  80},
        {"KnightOutpostEG",       &p.KnightOutpostEG,       0,  80},
        {"BishopOutpostMG",       &p.BishopOutpostMG,       0,  60},
        {"BishopOutpostEG",       &p.BishopOutpostEG,       0,  60},
        {"HangingPenaltyMG",      &p.HangingPenaltyMG,      0, 120},
    };
    const int N = int(sizeof(knobs) / sizeof(knobs[0]));

    double best = m;
    int sweep = 0;
    while (true) {
        ++sweep;
        bool changed = false;
        for (int i = 0; i < N; ++i) {
            int orig = *knobs[i].ptr;
            int bestVal = orig;
            double bestM = best;
            for (int step : {-4, -2, -1, 1, 2, 4}) {
                int v = orig + step;
                if (v < knobs[i].floor || v > knobs[i].ceil) continue;
                *knobs[i].ptr = v;
                double trial = mse(records, k);
                if (trial < bestM) { bestM = trial; bestVal = v; }
            }
            if (bestVal != orig) {
                *knobs[i].ptr = bestVal;
                std::printf("  sweep %d: %-22s %4d -> %4d   MSE %.6f -> %.6f\n",
                            sweep, knobs[i].name, orig, bestVal, best, bestM);
                best = bestM;
                changed = true;
            } else {
                *knobs[i].ptr = orig;
            }
        }
        if (!changed) break;
        if (sweep >= 8) {                   // safety cap
            std::printf("(reached sweep cap; stopping)\n");
            break;
        }
    }

    std::printf("\n=== TUNED VALUES (paste into src/eval_params.h) ===\n");
    for (int i = 0; i < N; ++i)
        std::printf("    int %-22s = %d;\n", knobs[i].name, *knobs[i].ptr);
    std::printf("Final MSE: %.6f (started %.6f, gain %.6f)\n", best, m, m - best);
    return 0;
}
