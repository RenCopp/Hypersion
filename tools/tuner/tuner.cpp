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

#ifdef _OPENMP
#include <omp.h>
#endif

#include "../../src/bitboard.h"
#include "../../src/eval_params.h"
#include "../../src/evaluate.h"
#include "../../src/position.h"
#include "../../src/zobrist.h"

// NNUE stubs — the tuner only uses Hypersion's classical evaluator and must
// not link nnue.cpp (the file pulls in ~1.2k LOC + 100 MB nets). Eval::evaluate
// short-circuits to classical when NNUE::is_loaded() returns false.
namespace hypersion::NNUE {
bool   is_loaded()                       { return false; }
hypersion::Value evaluate(const hypersion::Position&) { return hypersion::VALUE_ZERO; }
}

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
// OpenMP-parallelised: with N positions and T threads, each thread gets
// N/T records to evaluate (per-thread Position + StateInfo); per-thread
// partial sums are reduced at the end. Eval::evaluate is read-only on the
// Eval::params() globals during a tuner inner loop (we mutate params
// between mse() calls, not during), so position-shard parallelism is
// safe. ~6× speedup on a 6-core host.
double mse(const std::vector<LabeledPos>& records, double k) {
    double sum = 0.0;
    const long long N = (long long)records.size();
    if (N <= 0) return 0.0;

#ifdef _OPENMP
    #pragma omp parallel reduction(+:sum)
    {
        Position pos;
        StateInfo state;
        #pragma omp for schedule(static)
        for (long long i = 0; i < N; ++i) {
            pos.set(records[i].fen, &state);
            Value v = Eval::evaluate(pos);
            double cp = (pos.side_to_move() == WHITE) ? double(v) : -double(v);
            double p = sigmoid(cp, k);
            double d = p - records[i].result;
            sum += d * d;
        }
    }
#else
    Position pos;
    StateInfo state;
    for (long long i = 0; i < N; ++i) {
        pos.set(records[i].fen, &state);
        Value v = Eval::evaluate(pos);
        double cp = (pos.side_to_move() == WHITE) ? double(v) : -double(v);
        double p = sigmoid(cp, k);
        double d = p - records[i].result;
        sum += d * d;
    }
#endif
    return sum / double(N);
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
    bool        tune        = false;
    bool        psqt_only   = false;
    bool        pval_only   = false;
    bool        passed_only = false;
    bool        mob_only      = false;
    bool        threats_only  = false;
    bool        king_only     = false;
    bool        shelter_only  = false;
    bool        init_only     = false;
    bool        scale_only    = false;
    bool        new_only      = false;
    bool        part2_only    = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--in")           in_path      = argv[++i];
        else if (a == "--tune")         tune         = true;
        else if (a == "--psqt-only")    psqt_only    = true;
        else if (a == "--pval-only")    pval_only    = true;
        else if (a == "--passed-only")  passed_only  = true;
        else if (a == "--mob-only")     mob_only     = true;
        else if (a == "--threats-only") threats_only = true;
        else if (a == "--king-only")    king_only    = true;
        else if (a == "--shelter-only") shelter_only = true;
        else if (a == "--init-only")    init_only    = true;
        else if (a == "--scale-only")   scale_only   = true;
        else if (a == "--new-only")     new_only     = true;
        else if (a == "--part2-only")   part2_only   = true;
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                "Usage: tuner --in positions.txt [--tune] [--psqt-only | --pval-only]\n"
                "  --psqt-only restricts tuning to R10 PSQT scalar multipliers\n"
                "  --pval-only restricts tuning to R13 piece-value scalars\n"
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
    // Isolated PassedRankBonus tune (R14 / --passed-only).
    Knob passed_knobs[] = {
        {"PassedRank4",             &p.PassedRank4,             0, 200},
        {"PassedRank5",             &p.PassedRank5,             0, 400},
        {"PassedRank6",             &p.PassedRank6,             0, 500},
    };
    // Isolated R16 Threats tune (--threats-only). 11 knobs.
    Knob threats_knobs[] = {
        {"ThreatByMinor_Rook",      &p.ThreatByMinor_Rook,      0, 200},
        {"ThreatByMinor_Queen",     &p.ThreatByMinor_Queen,     0, 200},
        {"ThreatByRook_Queen",      &p.ThreatByRook_Queen,      0, 200},
        {"ThreatByPawn_KnightMG",   &p.ThreatByPawn_KnightMG,   0, 200},
        {"ThreatByPawn_BishopMG",   &p.ThreatByPawn_BishopMG,   0, 200},
        {"ThreatByPawn_RookMG",     &p.ThreatByPawn_RookMG,     0, 250},
        {"ThreatByPawn_QueenMG",    &p.ThreatByPawn_QueenMG,    0, 300},
        {"ThreatByPawn_KnightEG",   &p.ThreatByPawn_KnightEG,   0, 150},
        {"ThreatByPawn_BishopEG",   &p.ThreatByPawn_BishopEG,   0, 150},
        {"ThreatByPawn_RookEG",     &p.ThreatByPawn_RookEG,     0, 150},
        {"ThreatByPawn_QueenEG",    &p.ThreatByPawn_QueenEG,    0, 200},
    };
    // Isolated R17 King-attack-detail tune (--king-only). Ceilings
    // widened on SafeCheck after 16M tune pinned Knight/Rook at 100.
    Knob king_knobs[] = {
        {"KingAttacker_Knight",     &p.KingAttacker_Knight,     0, 200},
        {"KingAttacker_Bishop",     &p.KingAttacker_Bishop,     0, 200},
        {"KingAttacker_Rook",       &p.KingAttacker_Rook,       0, 200},
        {"KingAttacker_Queen",      &p.KingAttacker_Queen,      0, 100},
        {"SafeCheck_Knight",        &p.SafeCheck_Knight,        0, 200},
        {"SafeCheck_Bishop",        &p.SafeCheck_Bishop,        0, 150},
        {"SafeCheck_Rook",          &p.SafeCheck_Rook,          0, 200},
        {"SafeCheck_Queen",         &p.SafeCheck_Queen,         0, 150},
    };
    // Isolated R18 PawnShelter tune (--shelter-only). 3 knobs.
    Knob shelter_knobs[] = {
        {"PawnShelter_1Missing",    &p.PawnShelter_1Missing,    0,  50},
        {"PawnShelter_2Missing",    &p.PawnShelter_2Missing,    0,  80},
        {"PawnShelter_3Missing",    &p.PawnShelter_3Missing,    0, 150},
    };
    // Isolated R22 Initiative bonus tune (--init-only). 6 knobs.
    // Active values are SF7's; the coefficients are large because they
    // operate on a "complexity" sum that can range from ~-200 to +400.
    Knob init_knobs[] = {
        {"InitiativeOutflanking",   &p.InitiativeOutflanking,   0,  30},
        {"InitiativePawnCount",     &p.InitiativePawnCount,     0,  30},
        {"InitiativeBothFlanks",    &p.InitiativeBothFlanks,    0,  40},
        {"InitiativePureEndgame",   &p.InitiativePureEndgame,   0, 100},
        {"InitiativeOffset",        &p.InitiativeOffset,        0, 250},
        {"InitiativeScale",         &p.InitiativeScale,         0, 200},
    };
    // Isolated R25/R26 Endgame scale tune (--scale-only). 4 knobs.
    Knob scale_knobs[] = {
        {"OCBEgScale",              &p.OCBEgScale,             20, 100},
        {"WrongBishopRPScale",      &p.WrongBishopRPScale,      0,  60},
        {"KnightRPScale",           &p.KnightRPScale,           0,  80},
        {"KPKDrawScale",            &p.KPKDrawScale,            0,  30},
    };
    // R32-R37 Part-2 features (--part2-only). 10 knobs.
    // TIGHTER CEILINGS than the previous tune which regressed WAC -8.
    // The 16M master-game tune found values too large to preserve
    // tactical sharpness; constrain to magnitudes the tactics can absorb.
    Knob part2_knobs[] = {
        {"ConnectedPasserMG",       &p.ConnectedPasserMG,       0,  15},
        {"ConnectedPasserEG",       &p.ConnectedPasserEG,       0,  30},
        {"TradeDownBonusEG",        &p.TradeDownBonusEG,        0,   4},
        {"BadBishopBlockedMG",      &p.BadBishopBlockedMG,      0,   6},
        {"BadBishopBlockedEG",      &p.BadBishopBlockedEG,      0,   6},
        {"RookTrappedByKingMG",     &p.RookTrappedByKingMG,     0,  40},
        {"BackwardOnHalfOpenMG",    &p.BackwardOnHalfOpenMG,    0,  10},
        {"BackwardOnHalfOpenEG",    &p.BackwardOnHalfOpenEG,    0,  10},
        {"ImbalanceScale",          &p.ImbalanceScale,          0,  40},
    };
    // R27+ NEW tunable features (--new-only). All new features added in
    // this round so the tuner can find good values for them together.
    Knob new_knobs[] = {
        // R22 Initiative — re-tuned together with new features.
        {"InitiativeOutflanking",   &p.InitiativeOutflanking,   0,  30},
        {"InitiativePawnCount",     &p.InitiativePawnCount,     0,  30},
        {"InitiativeBothFlanks",    &p.InitiativeBothFlanks,    0,  40},
        {"InitiativePureEndgame",   &p.InitiativePureEndgame,   0, 100},
        {"InitiativeOffset",        &p.InitiativeOffset,        0, 250},
        {"InitiativeScale",         &p.InitiativeScale,         0, 200},
        // R23 KBNK scales.
        {"KBNKCornerScale",         &p.KBNKCornerScale,         0, 200},
        {"KBNKCloseScale",          &p.KBNKCloseScale,          0, 200},
        // R25 scaling.
        {"WrongBishopRPScale",      &p.WrongBishopRPScale,      0,  60},
        {"KnightRPScale",           &p.KnightRPScale,           0,  80},
        {"KPKDrawScale",            &p.KPKDrawScale,            0,  30},
        // R27 Knight rim.
        {"KnightRimPenaltyMG",      &p.KnightRimPenaltyMG,      0,  30},
        {"KnightRimPenaltyEG",      &p.KnightRimPenaltyEG,      0,  40},
        // R28 Pawn islands.
        {"PawnIslandPenaltyMG",     &p.PawnIslandPenaltyMG,     0,  20},
        {"PawnIslandPenaltyEG",     &p.PawnIslandPenaltyEG,     0,  30},
        // R29 Bishop pair × openness.
        {"BishopPairOpenScaleMG",   &p.BishopPairOpenScaleMG,   0, 200},
        {"BishopPairOpenScaleEG",   &p.BishopPairOpenScaleEG,   0, 200},
        // R30 Rook on 8th.
        {"RookOn8thEG",             &p.RookOn8thEG,             0,  80},
        // R31 Queen-king tropism.
        {"QueenKingTropismMG",      &p.QueenKingTropismMG,      0,  30},
        // R32 Connected passers.
        {"ConnectedPasserMG",       &p.ConnectedPasserMG,       0,  80},
        {"ConnectedPasserEG",       &p.ConnectedPasserEG,       0, 150},
        // R33 Trade-down bonus.
        {"TradeDownBonusEG",        &p.TradeDownBonusEG,        0,  30},
        // R34 Bad bishop blocked.
        {"BadBishopBlockedMG",      &p.BadBishopBlockedMG,      0,  20},
        {"BadBishopBlockedEG",      &p.BadBishopBlockedEG,      0,  30},
        // R35 Rook trapped by king.
        {"RookTrappedByKingMG",     &p.RookTrappedByKingMG,     0,  80},
        // R36 Backward on half-open.
        {"BackwardOnHalfOpenMG",    &p.BackwardOnHalfOpenMG,    0,  30},
        {"BackwardOnHalfOpenEG",    &p.BackwardOnHalfOpenEG,    0,  30},
    };
    // Isolated Mobility-scalar tune (R15 / --mob-only). Ceilings widened
    // for EG mobility after 16M tune pinned them at 196.
    Knob mob_knobs[] = {
        {"KnightMobScaleMG",        &p.KnightMobScaleMG,       50, 200},
        {"KnightMobScaleEG",        &p.KnightMobScaleEG,       50, 300},
        {"BishopMobScaleMG",        &p.BishopMobScaleMG,       50, 200},
        {"BishopMobScaleEG",        &p.BishopMobScaleEG,       50, 300},
        {"RookMobScaleMG",          &p.RookMobScaleMG,         50, 250},
        {"RookMobScaleEG",          &p.RookMobScaleEG,         50, 250},
        {"QueenMobScaleMG",         &p.QueenMobScaleMG,        50, 200},
        {"QueenMobScaleEG",         &p.QueenMobScaleEG,        50, 300},
    };
    // Isolated PieceValue-only tune (R13 / --pval-only). Tight ranges
    // because piece values are fundamental.
    Knob pval_knobs[] = {
        {"PawnValueScaleMG",        &p.PawnValueScaleMG,       90, 115},
        {"PawnValueScaleEG",        &p.PawnValueScaleEG,       90, 115},
        {"KnightValueScaleMG",      &p.KnightValueScaleMG,     85, 120},
        {"KnightValueScaleEG",      &p.KnightValueScaleEG,     85, 120},
        {"BishopValueScaleMG",      &p.BishopValueScaleMG,     85, 120},
        {"BishopValueScaleEG",      &p.BishopValueScaleEG,     85, 120},
        {"RookValueScaleMG",        &p.RookValueScaleMG,       85, 120},
        {"RookValueScaleEG",        &p.RookValueScaleEG,       85, 120},
        {"QueenValueScaleMG",       &p.QueenValueScaleMG,      85, 120},
        {"QueenValueScaleEG",       &p.QueenValueScaleEG,      85, 120},
    };
    // Isolated PSQT-only tune: R10's 12 scalars only. Used when the
    // user passes --psqt-only. Ceilings widened post-16M tune for
    // categories that pinned.
    Knob psqt_knobs[] = {
        {"PawnPSQTScaleMG",         &p.PawnPSQTScaleMG,        50, 200},
        {"PawnPSQTScaleEG",         &p.PawnPSQTScaleEG,        50, 250},
        {"KnightPSQTScaleMG",       &p.KnightPSQTScaleMG,      50, 250},
        {"KnightPSQTScaleEG",       &p.KnightPSQTScaleEG,      50, 250},
        {"BishopPSQTScaleMG",       &p.BishopPSQTScaleMG,      50, 300},
        {"BishopPSQTScaleEG",       &p.BishopPSQTScaleEG,      50, 300},
        {"RookPSQTScaleMG",         &p.RookPSQTScaleMG,        50, 300},
        {"RookPSQTScaleEG",         &p.RookPSQTScaleEG,        50, 300},
        {"QueenPSQTScaleMG",        &p.QueenPSQTScaleMG,       50, 200},
        {"QueenPSQTScaleEG",        &p.QueenPSQTScaleEG,       50, 300},
        {"KingPSQTScaleMG",         &p.KingPSQTScaleMG,        50, 200},
        {"KingPSQTScaleEG",         &p.KingPSQTScaleEG,        50, 250},
    };
    Knob knobs[] = {
        {"IsolatedPawnPenalty",     &p.IsolatedPawnPenalty,     0,  80},
        {"DoubledPawnPenalty",      &p.DoubledPawnPenalty,      0,  80},
        {"BackwardPawnPenalty",     &p.BackwardPawnPenalty,     0,  60},
        {"BishopPairBonusMG",       &p.BishopPairBonusMG,      -10, 120},
        {"BishopPairBonusEG",       &p.BishopPairBonusEG,      -10, 200},
        {"RookOpenFileMG",          &p.RookOpenFileMG,          0, 100},
        {"RookOpenFileEG",          &p.RookOpenFileEG,          0,  80},
        {"RookSemiOpenFileMG",      &p.RookSemiOpenFileMG,      0,  60},
        {"RookSemiOpenFileEG",      &p.RookSemiOpenFileEG,      0,  60},
        {"KnightOutpostMG",         &p.KnightOutpostMG,         0, 100},
        {"KnightOutpostEG",         &p.KnightOutpostEG,         0, 120},
        {"BishopOutpostMG",         &p.BishopOutpostMG,         0,  80},
        {"BishopOutpostEG",         &p.BishopOutpostEG,         0,  80},
        {"HangingPenaltyMG",        &p.HangingPenaltyMG,        0, 150},
        // New 2026-05-13 (Round 1 expansion):
        {"HangingPenaltyEG",        &p.HangingPenaltyEG,        0, 150},
        {"BishopPawnSCMG",          &p.BishopPawnSCMG,          0,  40},
        {"BishopPawnSCEG",          &p.BishopPawnSCEG,          0,  60},
        {"LongDiagBishopMG",        &p.LongDiagBishopMG,        0,  80},
        {"MinorBehindPawnMG",       &p.MinorBehindPawnMG,       0, 100},
        {"PassedKingEnemyDistEG",   &p.PassedKingEnemyDistEG,   0,  80},
        {"PassedKingOwnDistEG",     &p.PassedKingOwnDistEG,     0,  60},
        // Round 2 (2026-05-13). Ceilings widened in Round 8 after the 3M
        // master-game tune pinned Tempo at 60/50 (suggesting more signal
        // beyond the ceiling).
        {"TempoMG",                 &p.TempoMG,                 0, 120},
        {"TempoEG",                 &p.TempoEG,                 0, 100},
        {"TrappedBishopMG",         &p.TrappedBishopMG,         0, 400},
        {"TrappedBishopEG",         &p.TrappedBishopEG,         0, 400},
        {"PhalanxPawnMG",           &p.PhalanxPawnMG,           0,  60},
        {"SpaceAreaMG",             &p.SpaceAreaMG,             0,  50},
        // Round 3 (2026-05-13):
        {"RookOn7thMG",             &p.RookOn7thMG,             0, 100},
        {"RookOn7thEG",             &p.RookOn7thEG,             0, 200},
        {"CandidatePawnEG",         &p.CandidatePawnEG,         0,  80},
        {"ReachableOutpostMG",      &p.ReachableOutpostMG,      0,  60},
        {"PawnLeverMG",             &p.PawnLeverMG,             0,  40},
        // Round 4 (2026-05-13). Ceiling widened in Round 8 after 3M tune
        // pinned DoubledRookMG at 60.
        {"DoubledRookMG",           &p.DoubledRookMG,           0, 120},
        {"DoubledRookEG",           &p.DoubledRookEG,           0,  60},
        {"ConnectedRookMG",         &p.ConnectedRookMG,         0,  40},
        {"PawnStormMG",             &p.PawnStormMG,             0,  30},
        {"KnightVsBishopPawnsMG",   &p.KnightVsBishopPawnsMG,   0,  20},
        {"KnightVsBishopPawnsEG",   &p.KnightVsBishopPawnsEG,   0,  20},
        // Round 5 (2026-05-13) — mobility scalar multipliers. Ceilings
        // widened in Round 8 after the 3M tune pinned several at 192-196.
        {"KnightMobScaleMG",        &p.KnightMobScaleMG,       50, 300},
        {"KnightMobScaleEG",        &p.KnightMobScaleEG,       50, 300},
        {"BishopMobScaleMG",        &p.BishopMobScaleMG,       50, 300},
        {"BishopMobScaleEG",        &p.BishopMobScaleEG,       50, 300},
        {"RookMobScaleMG",          &p.RookMobScaleMG,         50, 300},
        {"RookMobScaleEG",          &p.RookMobScaleEG,         50, 300},
        {"QueenMobScaleMG",         &p.QueenMobScaleMG,        50, 300},
        {"QueenMobScaleEG",         &p.QueenMobScaleEG,        50, 300},
        // Round 6 (2026-05-13) — king attack refinements:
        {"RookOnKingFileMG",        &p.RookOnKingFileMG,        0,  60},
        {"BishopXrayQueenMG",       &p.BishopXrayQueenMG,       0,  40},
        {"KingSafetyScale",         &p.KingSafetyScale,        50, 400},
        {"OpenFilesNearKingMG",     &p.OpenFilesNearKingMG,     0,  40},
        // Round 7 (2026-05-13) — EG splits for previously MG-only features:
        {"PhalanxPawnEG",           &p.PhalanxPawnEG,           0,  60},
        {"PawnLeverEG",             &p.PawnLeverEG,             0,  40},
        {"ConnectedRookEG",         &p.ConnectedRookEG,         0,  40},
        {"ReachableOutpostEG",      &p.ReachableOutpostEG,      0,  60},
        // Round 10 (2026-05-13) — per-piece PSQT scalar multipliers.
        // Ceilings widened to 300 after 16M tune pinned Bishop/Rook/Queen
        // at the original 200 ceiling.
        {"PawnPSQTScaleMG",         &p.PawnPSQTScaleMG,        50, 200},
        {"PawnPSQTScaleEG",         &p.PawnPSQTScaleEG,        50, 250},
        {"KnightPSQTScaleMG",       &p.KnightPSQTScaleMG,      50, 250},
        {"KnightPSQTScaleEG",       &p.KnightPSQTScaleEG,      50, 250},
        {"BishopPSQTScaleMG",       &p.BishopPSQTScaleMG,      50, 300},
        {"BishopPSQTScaleEG",       &p.BishopPSQTScaleEG,      50, 300},
        {"RookPSQTScaleMG",         &p.RookPSQTScaleMG,        50, 300},
        {"RookPSQTScaleEG",         &p.RookPSQTScaleEG,        50, 300},
        {"QueenPSQTScaleMG",        &p.QueenPSQTScaleMG,       50, 200},
        {"QueenPSQTScaleEG",        &p.QueenPSQTScaleEG,       50, 300},
        {"KingPSQTScaleMG",         &p.KingPSQTScaleMG,        50, 200},
        {"KingPSQTScaleEG",         &p.KingPSQTScaleEG,        50, 250},
        // Round 11 (2026-05-13) — ConnectedPawnBonus rank-keyed:
        {"ConnectedPawnRank4",      &p.ConnectedPawnRank4,      0, 100},
        {"ConnectedPawnRank5",      &p.ConnectedPawnRank5,      0, 150},
        {"ConnectedPawnRank6",      &p.ConnectedPawnRank6,      0, 200},
        // Round 14 (2026-05-13) — PassedRankBonus rank-keyed:
        {"PassedRank4",             &p.PassedRank4,             0, 200},
        {"PassedRank5",             &p.PassedRank5,             0, 400},
        {"PassedRank6",             &p.PassedRank6,             0, 500},
        // Round 16 (2026-05-13) — Threat table entries:
        {"ThreatByMinor_Rook",      &p.ThreatByMinor_Rook,      0, 200},
        {"ThreatByMinor_Queen",     &p.ThreatByMinor_Queen,     0, 200},
        {"ThreatByRook_Queen",      &p.ThreatByRook_Queen,      0, 200},
        {"ThreatByPawn_KnightMG",   &p.ThreatByPawn_KnightMG,   0, 200},
        {"ThreatByPawn_BishopMG",   &p.ThreatByPawn_BishopMG,   0, 200},
        {"ThreatByPawn_RookMG",     &p.ThreatByPawn_RookMG,     0, 250},
        {"ThreatByPawn_QueenMG",    &p.ThreatByPawn_QueenMG,    0, 300},
        {"ThreatByPawn_KnightEG",   &p.ThreatByPawn_KnightEG,   0, 150},
        {"ThreatByPawn_BishopEG",   &p.ThreatByPawn_BishopEG,   0, 150},
        {"ThreatByPawn_RookEG",     &p.ThreatByPawn_RookEG,     0, 150},
        {"ThreatByPawn_QueenEG",    &p.ThreatByPawn_QueenEG,    0, 200},
        // Round 17 (2026-05-13) — KingAttacker + SafeCheck weights:
        {"KingAttacker_Knight",     &p.KingAttacker_Knight,     0, 200},
        {"KingAttacker_Bishop",     &p.KingAttacker_Bishop,     0, 150},
        {"KingAttacker_Rook",       &p.KingAttacker_Rook,       0, 150},
        {"KingAttacker_Queen",      &p.KingAttacker_Queen,      0,  60},
        {"SafeCheck_Knight",        &p.SafeCheck_Knight,        0, 100},
        {"SafeCheck_Bishop",        &p.SafeCheck_Bishop,        0,  80},
        {"SafeCheck_Rook",          &p.SafeCheck_Rook,          0, 100},
        {"SafeCheck_Queen",         &p.SafeCheck_Queen,         0,  80},
        // Round 18 (2026-05-13) — Pawn shelter detail:
        {"PawnShelter_1Missing",    &p.PawnShelter_1Missing,    0,  50},
        {"PawnShelter_2Missing",    &p.PawnShelter_2Missing,    0,  80},
        {"PawnShelter_3Missing",    &p.PawnShelter_3Missing,    0, 150},
        // Round 19 (2026-05-13) — Pawn penalty MG/EG splits:
        {"IsolatedPawnPenaltyEG",   &p.IsolatedPawnPenaltyEG,   0,  80},
        {"DoubledPawnPenaltyEG",    &p.DoubledPawnPenaltyEG,    0,  80},
        {"BackwardPawnPenaltyEG",   &p.BackwardPawnPenaltyEG,   0,  60},
        // Round 20 (2026-05-13) — Mop-up eval:
        {"MopUpKingCenter",         &p.MopUpKingCenter,         0,  60},
        {"MopUpKingDistance",       &p.MopUpKingDistance,       0,  30},
        // Round 21 (2026-05-13) — Opposite-colour bishop EG scale (%):
        {"OCBEgScale",              &p.OCBEgScale,             20, 100},
        // Round 22 (2026-05-13) — Initiative bonus (SF7-style):
        {"InitiativeOutflanking",   &p.InitiativeOutflanking,   0,  30},
        {"InitiativePawnCount",     &p.InitiativePawnCount,     0,  30},
        {"InitiativeBothFlanks",    &p.InitiativeBothFlanks,    0,  40},
        {"InitiativePureEndgame",   &p.InitiativePureEndgame,   0, 100},
        {"InitiativeOffset",        &p.InitiativeOffset,        0, 250},
        {"InitiativeScale",         &p.InitiativeScale,         0, 300},
        // Round 23 (2026-05-13) — KBNK mating drive:
        {"KBNKCornerScale",         &p.KBNKCornerScale,         0, 200},
        {"KBNKCloseScale",          &p.KBNKCloseScale,          0, 200},
        // Round 25 (2026-05-13) — Drawish endgame scaling (%):
        {"WrongBishopRPScale",      &p.WrongBishopRPScale,      0,  60},
        {"KnightRPScale",           &p.KnightRPScale,           0,  80},
        // Round 26 (2026-05-13) — KPK bitbase draw scaling (%):
        {"KPKDrawScale",            &p.KPKDrawScale,            0,  30},
        // Round 27 (2026-05-13) — Knight on rim penalty:
        {"KnightRimPenaltyMG",      &p.KnightRimPenaltyMG,      0,  30},
        {"KnightRimPenaltyEG",      &p.KnightRimPenaltyEG,      0,  40},
        // Round 28 (2026-05-13) — Pawn island count penalty:
        {"PawnIslandPenaltyMG",     &p.PawnIslandPenaltyMG,     0,  20},
        {"PawnIslandPenaltyEG",     &p.PawnIslandPenaltyEG,     0,  30},
        // Round 29 (2026-05-13) — Bishop pair × position openness:
        {"BishopPairOpenScaleMG",   &p.BishopPairOpenScaleMG,   0, 200},
        {"BishopPairOpenScaleEG",   &p.BishopPairOpenScaleEG,   0, 200},
        // Round 30 (2026-05-13) — Rook on 8th rank EG bonus:
        {"RookOn8thEG",             &p.RookOn8thEG,             0,  80},
        // Round 31 (2026-05-13) — Queen-king tropism MG:
        {"QueenKingTropismMG",      &p.QueenKingTropismMG,      0,  30},
        // Round 32 (2026-05-14) — Connected passers:
        {"ConnectedPasserMG",       &p.ConnectedPasserMG,       0,  80},
        {"ConnectedPasserEG",       &p.ConnectedPasserEG,       0, 150},
        // Round 33 (2026-05-14) — Trade-down bonus EG:
        {"TradeDownBonusEG",        &p.TradeDownBonusEG,        0,  30},
        // Round 34 (2026-05-14) — Bad bishop blocked by own pawns:
        {"BadBishopBlockedMG",      &p.BadBishopBlockedMG,      0,  20},
        {"BadBishopBlockedEG",      &p.BadBishopBlockedEG,      0,  30},
        // Round 35 (2026-05-14) — Rook trapped by own king:
        {"RookTrappedByKingMG",     &p.RookTrappedByKingMG,     0,  80},
        // Round 36 (2026-05-14) — Backward pawn on half-open file:
        {"BackwardOnHalfOpenMG",    &p.BackwardOnHalfOpenMG,    0,  30},
        {"BackwardOnHalfOpenEG",    &p.BackwardOnHalfOpenEG,    0,  30},
        // Round 37 (2026-05-14) — Imbalance polynomial scale (%):
        {"ImbalanceScale",          &p.ImbalanceScale,          0, 200},
    };
    // Select knobs array based on flag.
    Knob* active_knobs = shelter_only ? shelter_knobs
                       : king_only    ? king_knobs
                       : threats_only ? threats_knobs
                       : mob_only     ? mob_knobs
                       : passed_only  ? passed_knobs
                       : pval_only    ? pval_knobs
                       : psqt_only    ? psqt_knobs
                       : init_only    ? init_knobs
                       : scale_only   ? scale_knobs
                       : new_only     ? new_knobs
                       : part2_only   ? part2_knobs
                                      : knobs;
    const int N = shelter_only ? int(sizeof(shelter_knobs) / sizeof(shelter_knobs[0]))
                : king_only    ? int(sizeof(king_knobs)    / sizeof(king_knobs[0]))
                : threats_only ? int(sizeof(threats_knobs) / sizeof(threats_knobs[0]))
                : mob_only     ? int(sizeof(mob_knobs)     / sizeof(mob_knobs[0]))
                : passed_only  ? int(sizeof(passed_knobs)  / sizeof(passed_knobs[0]))
                : pval_only    ? int(sizeof(pval_knobs)    / sizeof(pval_knobs[0]))
                : psqt_only    ? int(sizeof(psqt_knobs)    / sizeof(psqt_knobs[0]))
                : init_only    ? int(sizeof(init_knobs)    / sizeof(init_knobs[0]))
                : scale_only   ? int(sizeof(scale_knobs)   / sizeof(scale_knobs[0]))
                : new_only     ? int(sizeof(new_knobs)     / sizeof(new_knobs[0]))
                : part2_only   ? int(sizeof(part2_knobs)   / sizeof(part2_knobs[0]))
                               : int(sizeof(knobs)         / sizeof(knobs[0]));
    if (shelter_only) std::printf("(--shelter-only: tuning %d shelter scalars)\n", N);
    else if (king_only) std::printf("(--king-only: tuning %d king-attack details)\n", N);
    else if (threats_only) std::printf("(--threats-only: tuning %d threat scalars)\n", N);
    else if (mob_only) std::printf("(--mob-only: tuning %d mobility scalars)\n", N);
    else if (passed_only) std::printf("(--passed-only: tuning %d passed-pawn ranks)\n", N);
    else if (pval_only) std::printf("(--pval-only: tuning %d piece-value scalars)\n", N);
    else if (psqt_only) std::printf("(--psqt-only: tuning %d PSQT scalars)\n", N);
    else if (init_only) std::printf("(--init-only: tuning %d R22 Initiative knobs)\n", N);
    else if (scale_only) std::printf("(--scale-only: tuning %d endgame scale knobs)\n", N);
    else if (new_only) std::printf("(--new-only: tuning %d R22-R26 knobs together)\n", N);
    else if (part2_only) std::printf("(--part2-only: tuning %d R32-R36 knobs)\n", N);

    // Clamp every starting value into its declared [floor, ceil] range.
    // Otherwise a value above ceil can't move down (all +/-{1,2,4} steps
    // also fall out of range), so tightening a ceiling silently has no
    // effect. Re-baseline MSE after the clamp so we report the correct
    // starting point.
    bool clamped = false;
    for (int i = 0; i < N; ++i) {
        if (*active_knobs[i].ptr < active_knobs[i].floor) {
            std::printf("  clamp: %-22s %4d -> %4d (floor)\n",
                        active_knobs[i].name, *active_knobs[i].ptr, active_knobs[i].floor);
            *active_knobs[i].ptr = active_knobs[i].floor;
            clamped = true;
        } else if (*active_knobs[i].ptr > active_knobs[i].ceil) {
            std::printf("  clamp: %-22s %4d -> %4d (ceil)\n",
                        active_knobs[i].name, *active_knobs[i].ptr, active_knobs[i].ceil);
            *active_knobs[i].ptr = active_knobs[i].ceil;
            clamped = true;
        }
    }
    if (clamped) {
        m = mse(records, k);
        std::printf("post-clamp MSE: %.6f\n", m);
    }

    double best = m;
    int sweep = 0;
    while (true) {
        ++sweep;
        bool changed = false;
        for (int i = 0; i < N; ++i) {
            int orig = *active_knobs[i].ptr;
            int bestVal = orig;
            double bestM = best;
            for (int step : {-4, -2, -1, 1, 2, 4}) {
                int v = orig + step;
                if (v < active_knobs[i].floor || v > active_knobs[i].ceil) continue;
                *active_knobs[i].ptr = v;
                double trial = mse(records, k);
                if (trial < bestM) { bestM = trial; bestVal = v; }
            }
            if (bestVal != orig) {
                *active_knobs[i].ptr = bestVal;
                std::printf("  sweep %d: %-22s %4d -> %4d   MSE %.6f -> %.6f\n",
                            sweep, active_knobs[i].name, orig, bestVal, best, bestM);
                std::fflush(stdout);   // unbuffered progress so external pollers see it
                best = bestM;
                changed = true;
            } else {
                *active_knobs[i].ptr = orig;
            }
        }
        if (!changed) break;
        if (sweep >= 24) {                  // safety cap (raised 8 -> 24:
                                            // 8-sweep cap was hitting the
                                            // limit while MSE still falling)
            std::printf("(reached sweep cap; stopping)\n");
            break;
        }
    }

    std::printf("\n=== TUNED VALUES (paste into src/eval_params.h) ===\n");
    for (int i = 0; i < N; ++i)
        std::printf("    int %-22s = %d;\n", active_knobs[i].name, *active_knobs[i].ptr);
    std::printf("Final MSE: %.6f (started %.6f, gain %.6f)\n", best, m, m - best);
    return 0;
}
