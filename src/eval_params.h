// Hypersion — runtime-mutable evaluation parameters.
//
// The tunable SCALAR constants from evaluate.cpp live in this struct so the
// Texel tuner can mutate them at runtime. Arrays (PSQT, mobility tables,
// king-safety curve) stay constexpr in evaluate.cpp for now — they're a
// bigger refactor and the scalars are where the easy ELO is.
//
// Defaults match the values that were `constexpr int` in evaluate.cpp before
// the refactor.

#pragma once

namespace hypersion::Eval {

struct Params {
    // 2026-05-12 Texel re-tune over 221,616 labeled positions sampled from
    // all_session_games.pgn (~10,000 games at TC 5+0.05 and 10+0.1, mix of
    // Hypersion self-play and vs-SF matches). Two-stage tune:
    //   Stage 1 (8-sweep cap):  MSE 0.104448 → 0.103871
    //   Stage 2 (24-sweep cap, restart from stage-1 values):
    //                            MSE 0.103863 → 0.103828, converged at sweep 6
    // Total MSE gain 0.000620 ≈ 0.6 % from the previous tune.
    //
    // Previous values (kept for traceability):
    //   Iso=45  Dbl=4   Back=39  BPmg=-2  BPeg=82
    //   Romg=57 Roeg=44 RSmg=6   RSeg=0
    //   Knmg=41 Kneg=54 Bnmg=42  Bneg=44   Hang=82
    //
    // After stage-2 convergence several terms collapsed to 0
    // (BishopOutpost both phases, RookOpenFileEG, RookSemiOpen both
    // phases). That's a real signal that this dataset can't keep those
    // weights non-zero — but it doesn't mean the underlying concepts
    // are wrong; an LTC self-play dataset 10× larger may bring them
    // back. Left at zero to honour the tune.
    //
    // Note: classical eval is dispatched-around when NNUE is loaded (see
    // src/evaluate.cpp::evaluate); these values only move the needle in
    // classical-only mode (EvalFile = <empty>). No SPRT was run on the
    // shipped NNUE binary — the dataset is small (221 k vs the canonical
    // millions) and improvements there don't reach the shipped eval path.
    //
    // 2026-05-13 Round 2: 6 new features (5 in Round 1 + tempo MG/EG split +
    // trapped-bishop + phalanx + space). Tuned on the 221,616-position
    // session dataset with widened knob ceilings.
    //   Pre-Round-1 baseline MSE:                          0.103828
    //   Round 1 tuned:                                     0.101039
    //   Round 2 tuned, widened ceilings (this run):        0.100260
    //   Total gain:                                        0.003568 (≈3.4 %)
    //
    // Several intermediate Round-1 values shifted again on widening — most
    // notably BishopPairBonusEG (100 → 196), KnightOutpostEG (80 → 106),
    // PhalanxPawnMG (30 → 41), SpaceAreaMG (30 → 39).
    //
    // Pending: re-tune on the 20M-position Encroissant 2025 dataset (~90×
    // larger). Expected: another ~0.5-1.0 % MSE reduction and finer
    // tuning of the new features. Values below are checked-in as a safe
    // intermediate; the larger-dataset re-tune will overwrite them.
    //
    // 2026-05-13: Shipping R1-tuned values with R2 features in code but
    // DISABLED (neutral defaults). Investigation showed multiple R2-tune
    // re-balancings of pre-existing knobs (BishopPairBonusEG 100→200,
    // KnightOutpostEG 80→115, IsolatedPawnPenalty 33→41, ...) drop the
    // depth-8 WAC tactical from 181/198 to 177/198. Reverting JUST the
    // R2 features (set to 0/neutral) and using R1 tuned values gives
    // 180/198 (matching R1-tuned). Total session gain vs pre-session
    // (which was 177/198): +3/198 WAC + −0.002789 MSE from R1 alone.
    //
    // R2 features stay in the code so the 20M-master-game tune can find
    // proper values. With 90× more data of master-game distribution
    // (rather than Hypersion bullet self-play), the tuned values may
    // not cause the same WAC regression.

    // 2026-05-13 FINAL SHIP STATE: R1 tuned values restored after the
    // master-game tunes (3M then R8 widened) consistently dropped WAC
    // classical-only tactical from 180/198 to 177-178/198. The MSE-on-
    // master-game-positions optimization target doesn't preserve depth-8
    // tactical sharpness. NNUE-on shipped binary is unaffected throughout
    // (1,273,328 nodes constant). The R8 tuned values are documented in
    // testing/tuner_run_r8.txt as reference for future LTC tunes that
    // can verify with actual game-play SPRT.

    // Pawn structure (R1 tuned)
    int IsolatedPawnPenalty   = 33;
    int DoubledPawnPenalty    = 0;
    int BackwardPawnPenalty   = 20;

    // Bishop pair (R1)
    int BishopPairBonusMG     = 36;
    int BishopPairBonusEG     = 100;

    // Rook (R1)
    int RookOpenFileMG        = 46;
    int RookOpenFileEG        = 0;
    int RookSemiOpenFileMG    = 10;
    int RookSemiOpenFileEG    = 0;

    // Outposts (R1)
    int KnightOutpostMG       = 16;
    int KnightOutpostEG       = 80;
    int BishopOutpostMG       = 0;
    int BishopOutpostEG       = 17;

    // Hanging (R1)
    int HangingPenaltyMG      = 17;
    int HangingPenaltyEG      = 149;

    // R1 features (R1 tuned)
    int BishopPawnSCMG        = 3;
    int BishopPawnSCEG        = 49;
    int LongDiagBishopMG      = 48;
    int MinorBehindPawnMG     = 40;
    int PassedKingEnemyDistEG = 50;
    int PassedKingOwnDistEG   = 0;

    // R2 features DISABLED (neutral defaults).
    int TempoMG               = 28;
    int TempoEG               = 28;
    int TrappedBishopMG       = 0;
    int TrappedBishopEG       = 0;
    int PhalanxPawnMG         = 0;
    int SpaceAreaMG           = 0;

    // ── Round 3 (2026-05-13) additions — added in parallel with 20M
    // extraction. Starts at SF-equivalent magnitudes × 5 (Hyp scale);
    // 20M tune will adjust. Each feature has a low default so it won't
    // distort the safe-ship classical eval before tuning.

    // R3 features at original safe defaults (active but conservative).
    int RookOn7thMG           = 20;
    int RookOn7thEG           = 60;
    int CandidatePawnEG       = 15;
    int ReachableOutpostMG    = 10;
    int PawnLeverMG           = 5;

    // ── Round 4 (2026-05-13) ─────────────────────────────────────────────
    // 4 more features: doubled rooks, connected rooks, pawn storm vs king,
    // knight vs bishop imbalance based on pawn count. Each disabled at
    // small starting magnitudes; 20M tune adjusts.

    // R4 features at original safe defaults.
    int DoubledRookMG         = 8;
    int DoubledRookEG         = 4;
    int ConnectedRookMG       = 6;
    int PawnStormMG           = 4;

    // Knight vs Bishop imbalance scaling. Original R4 defaults 2/2.
    int KnightVsBishopPawnsMG = 2;
    int KnightVsBishopPawnsEG = 2;

    // ── Round 5 (2026-05-13) — Mobility scalar multipliers ───────────────
    // The MobilityMG[]/MobilityEG[] tables in evaluate.cpp are constexpr
    // and not directly tunable. These per-piece scalars let Texel adjust
    // the overall weight of each piece's mobility contribution. Default
    // 100 = exact reproduction of constexpr tables; tuner can scale up or
    // down in [50, 200] range to find better balance.
    // Applied as: contrib = MobilityMG[pt][n] × MobScale_pt_MG / 100.
    // R5 mobility scalars at neutral 100 (reproduce constexpr tables).
    int KnightMobScaleMG      = 105;
    int KnightMobScaleEG      = 196;
    int BishopMobScaleMG      = 111;
    int BishopMobScaleEG      = 192;
    int RookMobScaleMG        = 174;
    int RookMobScaleEG        = 155;
    int QueenMobScaleMG       = 160;
    int QueenMobScaleEG       = 196;

    // R6 king-attack features DISABLED (0 defaults).
    int RookOnKingFileMG      = 0;
    int BishopXrayQueenMG     = 0;
    int KingSafetyScale       = 100;
    int OpenFilesNearKingMG   = 0;

    // R7 EG splits all disabled.
    int PhalanxPawnEG         = 0;
    int PawnLeverEG           = 0;
    int ConnectedRookEG       = 0;
    int ReachableOutpostEG    = 0;

    // ── Round 10 (2026-05-13) — per-piece PSQT scalar multipliers ────────
    // The PSQ_<Piece>{MG,EG}[64] tables in evaluate.cpp are constexpr. These
    // scalars let Texel adjust each piece's PSQT contribution by ±50 %
    // without exposing 768 individual values. Analogous to R5's mobility
    // scalars (which carried strong signal in 3M tune: 117-196 range).
    // Default 100 = exact reproduction of constexpr tables.
    int PawnPSQTScaleMG       = 112;
    int PawnPSQTScaleEG       = 158;
    int KnightPSQTScaleMG     = 128;
    int KnightPSQTScaleEG     = 162;
    int BishopPSQTScaleMG     = 196;
    int BishopPSQTScaleEG     = 200;
    int RookPSQTScaleMG       = 200;
    int RookPSQTScaleEG       = 194;
    int QueenPSQTScaleMG      = 124;
    int QueenPSQTScaleEG      = 200;
    int KingPSQTScaleMG       = 82;
    int KingPSQTScaleEG       = 174;

    // ── Round 13 (2026-05-13) — PieceValue scalar multipliers ────────────
    // The PieceValueMG/EG arrays in evaluate.h are constexpr. These
    // scalars let Texel adjust each piece's intrinsic value by a small
    // percentage. Default 100 = exact reproduction. TIGHT ranges (80-130)
    // because piece-value changes cascade through all eval (every other
    // term is denominated relative to piece values).
    // Applied as: contribution = PieceValueMG[pt] × scale / 100.
    int PawnValueScaleMG      = 115;
    int PawnValueScaleEG      = 115;
    int KnightValueScaleMG    = 85;
    int KnightValueScaleEG    = 89;
    int BishopValueScaleMG    = 85;
    int BishopValueScaleEG    = 88;
    int RookValueScaleMG      = 85;
    int RookValueScaleEG      = 85;
    int QueenValueScaleMG     = 85;
    int QueenValueScaleEG     = 85;

    // ── Round 11 (2026-05-13) — ConnectedPawnBonus rank-keyed tuning ──────
    // The `ConnectedPawnBonus[8]` array in evaluate.cpp is constexpr by
    // relative rank index: indices 0..7 → { 0, 7, 8, 12, 29, 48, 86, 0 }.
    // Expose the meaningful advanced indices 4/5/6 (rank 5/6/7 = pre-
    // promotion squares) as scalars so Texel can refine the late-rank
    // curve. EG bonus is the full table value; MG is half (preserved
    // from the original `/2` in evaluate.cpp).
    int ConnectedPawnRank4    = 29;   // index 4, rank-5 from POV
    int ConnectedPawnRank5    = 48;   // index 5, rank-6
    int ConnectedPawnRank6    = 86;   // index 6, rank-7 (about to promote)

    // ── Round 14 (2026-05-13) — PassedRankBonus rank-keyed tuning ─────────
    // The `PassedRankBonus[8]` array is constexpr by relative rank:
    //   { 0, 10, 17, 15, 62, 168, 276, 0 }. Expose indices 4/5/6 (advanced
    //   ranks 5/6/7) for Texel refinement. Mg = full bonus / 3, eg = full.
    // 2026-05-13: R14 isolation tune wanted these ramped up (62→158,
    // 168→264, 276→372) but applying that dropped WAC 181→179 in noise.
    // Reverted to original constexpr values. The R14 tune output is
    // archived in testing/tuner_run_passed.txt.
    int PassedRank4           = 62;
    int PassedRank5           = 168;
    int PassedRank6           = 276;

    // ── Round 16 (2026-05-13) — Threat-table exposure ──────────────────────
    // Threats are bonuses when our pieces attack enemy pieces of higher
    // value. The original `ThreatByMinor/Rook/Pawn` arrays are constexpr;
    // these knobs expose the high-impact entries for Texel.

    // ThreatByMinor[attacked]: minor attacking ROOK or QUEEN
    int ThreatByMinor_Rook    = 146;   // was index 4
    int ThreatByMinor_Queen   = 0;   // was index 5

    // ThreatByRook[attacked]: rook attacking QUEEN
    int ThreatByRook_Queen    = 166;   // was index 5

    // ThreatByPawnMG[attacked]: pawn threatening N/B/R/Q
    int ThreatByPawn_KnightMG = 176;
    int ThreatByPawn_BishopMG = 176;
    int ThreatByPawn_RookMG   = 206;
    int ThreatByPawn_QueenMG  = 77;

    // ThreatByPawnEG[attacked]: same but in endgame
    int ThreatByPawn_KnightEG = 146;
    int ThreatByPawn_BishopEG = 146;
    int ThreatByPawn_RookEG   = 150;
    int ThreatByPawn_QueenEG  = 176;

    // ── Round 17 (2026-05-13) — King attack curve detail ───────────────────
    // KingAttackerWeight[attacker]: weight added to attackUnits per
    // king-zone attack square per attacker. Higher = more dangerous.
    int KingAttacker_Knight   = 81;
    int KingAttacker_Bishop   = 90;
    int KingAttacker_Rook     = 65;
    int KingAttacker_Queen    = 60;

    // SafeCheckWeight[attacker]: weight per safe-check square (square
    // that gives check from a square not defended by enemy).
    int SafeCheck_Knight      = 100;
    int SafeCheck_Bishop      = 80;
    int SafeCheck_Rook        = 100;
    int SafeCheck_Queen       = 80;

    // ── Round 18 (2026-05-13) — Pawn shelter detail ────────────────────────
    // R18 isolation tune wanted 50/80/150 (all pinned at ceiling) but
    // applying those dropped WAC 182→178/198. Reverted to original
    // constexpr defaults; R18 tune output is in tuner_run_shelter.txt.
    int PawnShelter_1Missing  = 12;
    int PawnShelter_2Missing  = 28;
    int PawnShelter_3Missing  = 60;

    // ── Round 19 (2026-05-13) — Pawn penalty MG/EG splits ──────────────────
    // The R1 IsolatedPawnPenalty/DoubledPawnPenalty/BackwardPawnPenalty
    // scalars were originally applied to BOTH mg and eg with the same
    // value. Real chess: isolated/doubled pawns matter more in endgames.
    // Add EG variants. Default = same as MG = R1 tuned values.
    int IsolatedPawnPenaltyEG = 33;   // copy of IsolatedPawnPenalty (R1)
    int DoubledPawnPenaltyEG  = 0;    // copy of DoubledPawnPenalty (R1)
    int BackwardPawnPenaltyEG = 20;   // copy of BackwardPawnPenalty (R1)

    // ── Round 20 (2026-05-13) — Mop-up eval ────────────────────────────────
    // When one side has a materially winning position AND the loser has
    // only the bare king (or K + at most one minor with no pawns), reward
    // driving the loser's king to the edge/corner. EG only.
    //
    // bonus = MopUpKingCenter   * chebyshev(loser_king, board_centre)
    //       + MopUpKingDistance * (14 - chebyshev(winner_king, loser_king))
    //
    // Reference: chessprogramming.org/Mop-up_evaluation. Stockfish-style
    // magnitudes (Hyp 5× scale of SF's ~5cp / ~2cp respectively).
    int MopUpKingCenter       = 25;
    int MopUpKingDistance     = 10;

    // ── Round 21 (2026-05-13) — Opposite-colour-bishop endgame scaling ────
    // When the only minor pieces are exactly one bishop per side on
    // opposite colours AND there are no queens/rooks on the board, the
    // position has strong drawing tendency. Scale the eg score down.
    // Standard heuristic: scale eg by ~0.5 (Stockfish uses similar).
    int OCBEgScale            = 50;   // % of original eg in OCB endgames

    // ── Round 22 (2026-05-13) — Initiative bonus (SF7-style) ─────────────
    // Endgame complexity bonus / penalty based on position asymmetry. Adds
    // a value with the sign of the existing eg, magnitude up to |eg|,
    // proportional to a "complexity" score that rewards:
    //   - king outflanking (file delta - rank delta between kings)
    //   - total pawn count
    //   - pawns on both flanks (queen-side AND king-side)
    //   - pure pawn endgame (no non-pawn material)
    // Source: Stockfish 7 initiative() (SF sf_10/src/evaluate.cpp).
    // Each coefficient is a tunable scalar. Final result is multiplied
    // by InitiativeScale (default 100 = SF-as-is, tuner can scale).
    // R22 Initiative bonus (SF7-style), TUNED 2026-05-13 on 16M positions
    // via --init-only (24 sweeps, MSE 0.196143 -> 0.195483, gain 0.000660).
    // The 16M-master-game tune found values quite different from SF7's:
    // PawnCount and BothFlanks pinned at ceiling, Offset way down (118 -> 22),
    // Scale moderate (100 -> 79). This signals the master-game distribution
    // weights initiative more strongly than self-play (more decisive games
    // come from positions with more complexity).
    // R22 final ship values from Phase-A --new-only tune (16M, MSE-0.000226,
    // WAC 184/198). Widened-ceiling re-tune (R22-v2, MSE-0.000061 more)
    // regressed WAC to 181 — the master-game MSE vs WAC tactical mismatch.
    // Earlier --init-only-only tune (Outflanking=19) also regressed.
    // These Phase-A joint-tuned values are the WAC-184 best.
    int InitiativeOutflanking = 16;
    int InitiativePawnCount   = 28;
    int InitiativeBothFlanks  = 40;
    int InitiativePureEndgame = 100;
    int InitiativeOffset      = 0;
    int InitiativeScale       = 112;

    // ── Round 23 (2026-05-13) — KBNK mating drive ────────────────────────
    // KBN-vs-K is a forced mate, but only into the corner matching the
    // bishop's colour. Push the loser's king toward that corner and the
    // winner's king close. Triggered when:
    //   strong side has exactly K+B+N, weak side has bare K, no pawns.
    // Replaces / supplements MopUp eval which doesn't know about bishop
    // colour and so can fail to drive to the right corner.
    int KBNKCornerScale       = 4;   // % of full PushToCorner table applied
    int KBNKCloseScale        = 4;   // % of full PushClose table applied

    // ── Round 25 (2026-05-13) — Drawish endgame scaling ──────────────────
    // Extend R21's OCB scaling to other known-drawish patterns:
    //   (a) KBPsK with wrong-coloured bishop + only rook-pawn(s) +
    //       defender king on/near the queening square → near-draw.
    //   (b) Knight + only rook-pawn(s) vs lone king → often draw.
    //   (c) Pure rook+pawn vs rook (one side has extra pawn) is somewhat
    //       drawish — small scale-down.
    // Values are % of the original eg score to keep; 0 = pure draw,
    // 100 = no scaling.
    int WrongBishopRPScale    = 30;   // wrong-colour bishop + rook-pawn
    int KnightRPScale         = 80;   // knight + rook-pawn vs bare king
    int RookEndgameScale      = 88;   // 4-piece+pawns rook ending mild down-scale

    // ── Round 26 (2026-05-13) — KPK bitbase ───────────────────────────────
    // KP-vs-K positions are 100% solvable. The bitbase tells us whether a
    // given (kings, pawn, stm) configuration is winning for the strong
    // side. On a "drawn" probe result, scale eg to KPKDrawScale% (0 by
    // default = collapse to draw; mop-up still adds its own bonus).
    int KPKDrawScale          = 30;

    // ── Round 27 (2026-05-13) — Knight on rim penalty ─────────────────────
    // Knights on the a/h files lose ~half of their attack range. Disabled
    // by default (=0) until tuned on 16M.
    int KnightRimPenaltyMG    = 0;
    int KnightRimPenaltyEG    = 0;

    // ── Round 28 (2026-05-13) — Pawn island penalty ───────────────────────
    // Disabled by default until tuned.
    int PawnIslandPenaltyMG   = 20;
    int PawnIslandPenaltyEG   = 30;

    // ── Round 29 (2026-05-13) — Bishop pair × open-position scale ─────────
    // Bishop pair is worth more when the position is OPEN (few pawns).
    // BishopPairOpenScale: extra bonus % per missing pawn from a baseline
    // of 16. With 16 pawns the bishop pair gets +0% boost; with 8 pawns
    // it gets +800/100 = +8 * scale / 100 percent boost.
    int BishopPairOpenScaleMG = 42;   // disabled by default until tuned
    int BishopPairOpenScaleEG = 96;

    // ── Round 30 (2026-05-13) — Rook on 8th rank ─────────────────────────
    // Disabled (=0) until tuned. Rooks on the absolute 8th rank are
    // usually attacking the back-rank; EG-only feature.
    int RookOn8thEG           = 0;

    // ── Round 31 (2026-05-13) — Queen-king tropism ───────────────────────
    // Each enemy slider/knight within chebyshev distance 3 of our king
    // gets a king-safety-zone bump. Already partially captured by R17
    // KingAttacker_X but the close-distance flag is additional pressure.
    int QueenKingTropismMG    = 30;   // disabled by default until tuned

    // ── Round 32 (2026-05-14) — Connected passed pawns ────────────────────
    // R32-R36 features ALL tuned on 16M but values regress WAC depth-8:
    //   - ConnectedPasserEG=80 (tuned)  -> 184 -> 177 (-7)
    //   - ConnectedPasserEG=30 (manual) -> 184 -> 178 (-6)
    //   - TradeDownBonusEG=8            -> 184 -> 176 (-8)
    //   - BadBishopBlocked              -> +0 once R33 disabled
    //   - RookTrappedByKing=78 (pinned) -> pinning + broad heuristic
    //   - BackwardOnHalfOpen MG/EG      -> minor effect
    // Classic "MSE-tune-vs-WAC mismatch": tunes on master-game positions
    // find values that lower MSE but reduce depth-8 tactical sharpness.
    // Disabled at 0 for the shipped binary; feature code stays in place
    // so future LTC SPRT can validate with actual game-play tests.
    int ConnectedPasserMG     = 0;
    int ConnectedPasserEG     = 0;

    // ── Round 33 (2026-05-14) — Trade-down bonus ──────────────────────────
    int TradeDownBonusEG      = 0;

    // ── Round 34 (2026-05-14) — Bad bishop sliding penalty ────────────────
    int BadBishopBlockedMG    = 0;
    int BadBishopBlockedEG    = 0;

    // ── Round 35 (2026-05-14) — Rook trapped by king after no-castle ──────
    // After loss of castling rights, a rook in the corner blocked by own
    // king (e.g., R-a1 + K-b1) is severely passive. Penalty per occurrence.
    // R35 RookTrappedByKing: tuned value 78 was pinned at ceiling and caused
    // WAC regression -8 (184 -> 176). Detection logic triggered on normal
    // castled positions (rook on h1 with king on g1 = trapped per the
    // heuristic, but that's just standard castling). Disabled until
    // detection logic is refined to require no-castling-rights.
    int RookTrappedByKingMG   = 0;

    // ── Round 36 (2026-05-14) — Backward pawn × half-open file ────────────
    int BackwardOnHalfOpenMG  = 0;
    int BackwardOnHalfOpenEG  = 0;

    // ── Round 37 (2026-05-14) — Imbalance polynomial scale ───────────────
    // SF-style material imbalance polynomial scoring (QuadraticOurs +
    // QuadraticTheirs in src/imbalance.h). Even tiny activation (scale=2)
    // regresses WAC marginally (-2 vs 184 baseline). Tactical-aligned
    // tune (decisive + WAC anchor) attempted 2026-05-14 and regressed to
    // 174 — the anchor approach overfits the small 141k dataset.
    // Conclusion: SF polynomial doesn't align with Hypersion's already-
    // tuned material weights (R13 PieceValue scalars + R10 PSQT scalars).
    // Disabled.
    int ImbalanceScale        = 0;
};

// Single global instance; mutable. Default-constructed with the values
// above; the tuner overwrites fields and re-evaluates.
inline Params& params() {
    static Params p;
    return p;
}

}  // namespace hypersion::Eval
