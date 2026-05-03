"""Drive Stockfish to ground-truth Hypersion's critical losing moves.

For each loss in the gauntlet PGNs:
  1. Walk the game move-by-move and parse Hypersion's per-move eval from
     PGN comments.
  2. Identify the "critical move" — the move where Hypersion's own eval
     dropped most steeply.
  3. At the critical position (before the move), ask Stockfish at fixed
     depth: what is the BEST eval available, and what eval does Hypersion's
     ACTUAL move yield?
  4. Classify:
        - True blunder      : best - actual >= 200 cp (good move existed)
        - Position lost     : best <= -300 cp regardless (nothing helped)
        - Horizon effect    : Hypersion's eval just caught up to truth
        - Borderline        : 50 cp <= gap < 200 cp
  5. Also record piece count at the critical position (relevant to whether
     Syzygy 3-4-5 tablebases would have helped).

Run: py -3 stockfish_blunder_check.py
"""

import chess
import chess.engine
import chess.pgn
import re
import sys
from pathlib import Path
from collections import Counter

PGNS = [
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_sf2400.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_obsidian.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_alexandria.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_rubichess.pgn",
]
HYPER_NAME = "Hyp_LMR"
STOCKFISH = r"C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe"
SF_DEPTH = 18  # ground truth depth — fast enough on AVX2, accurate enough

EVAL_RE = re.compile(r'^([+\-]?)(?:M(\d+)|(\d+(?:\.\d+)?))$')

def parse_eval(s):
    """Return centipawns, or None on parse failure. Mate flags clamped to ±1500."""
    m = EVAL_RE.match(s)
    if not m:
        return None
    sign_str, mate_str, num_str = m.groups()
    sign = -1 if sign_str == '-' else 1
    if mate_str is not None:
        return sign * 1500
    if num_str is not None:
        return int(round(sign * float(num_str) * 100))
    return None

def critical_move_index(game, hyper_color):
    """Walk game, build list of (ply, hyper_eval_cp, board_before, move).
    Return the ply with steepest eval drop (most negative delta)."""
    board = game.board()
    hyper_records = []
    for ply, node in enumerate(game.mainline()):
        side = chess.WHITE if (ply % 2 == 0) else chess.BLACK
        move = node.move
        if side == hyper_color:
            comment = node.comment or ""
            # Comment looks like:  -2.40/12 0.38s
            mc = re.match(r'\s*([+\-]?M?\d+(?:\.\d+)?)\s*/\s*(\d+)', comment)
            if mc:
                eval_cp = parse_eval(mc.group(1))
                if eval_cp is not None:
                    hyper_records.append({
                        "ply": ply,
                        "fullmove": ply // 2 + 1,
                        "eval_cp": eval_cp,
                        "fen_before": board.fen(),
                        "move": move,
                        "san": board.san(move),
                    })
        board.push(move)

    if len(hyper_records) < 2:
        return None

    max_drop = 0
    crit = None
    for i in range(1, len(hyper_records)):
        delta = hyper_records[i]["eval_cp"] - hyper_records[i-1]["eval_cp"]
        if delta < max_drop:
            max_drop = delta
            crit = hyper_records[i]

    if crit is None:
        return None
    crit["max_drop"] = max_drop
    return crit

def piece_count(fen):
    parts = fen.split()
    pieces = parts[0]
    return sum(1 for c in pieces if c.isalpha())

def analyze_with_sf(engine, fen_before, hypersion_move, depth):
    """Return dict with best_eval (Stockfish's), hyper_eval (after Hyp's move)."""
    board = chess.Board(fen_before)
    # Best move analysis
    info_best = engine.analyse(board, chess.engine.Limit(depth=depth))
    best_score = info_best["score"].white()
    best_move = info_best.get("pv", [None])[0]
    # Score is from White's perspective in python-chess; we want side-to-move POV
    stm_sign = 1 if board.turn == chess.WHITE else -1
    best_eval_stm = stm_sign * (best_score.score(mate_score=15000) or 0)

    # Hypersion's actual move
    board.push(hypersion_move)
    info_actual = engine.analyse(board, chess.engine.Limit(depth=depth))
    actual_score = info_actual["score"].white()
    # After hyper moves, it's the OPPONENT's turn — eval is now opponent's POV.
    # Convert back to Hypersion's POV: negate.
    opp_sign = 1 if board.turn == chess.WHITE else -1
    hyper_eval_stm = -(opp_sign * (actual_score.score(mate_score=15000) or 0))

    return {
        "best_move": best_move,
        "best_eval_stm": best_eval_stm,
        "hyper_eval_stm": hyper_eval_stm,
        "delta": best_eval_stm - hyper_eval_stm,
    }

def classify(best_eval, gap):
    """Classify a critical move based on Stockfish ground truth."""
    if best_eval <= -300:
        return "already-lost"
    if gap >= 200:
        return "true-blunder"
    if gap >= 50:
        return "borderline"
    return "horizon-only"   # Hypersion's eval was right; just deeper search caught up

def main():
    losses = []
    for pgn_path in PGNS:
        opp = Path(pgn_path).stem.replace("gauntlet2_vs_", "")
        with open(pgn_path) as f:
            while True:
                game = chess.pgn.read_game(f)
                if game is None:
                    break
                white = game.headers.get("White", "")
                black = game.headers.get("Black", "")
                result = game.headers.get("Result", "*")
                if HYPER_NAME == white:
                    hyper_color = chess.WHITE
                elif HYPER_NAME == black:
                    hyper_color = chess.BLACK
                else:
                    continue
                hyper_lost = (result == "0-1" and hyper_color == chess.WHITE) or \
                             (result == "1-0" and hyper_color == chess.BLACK)
                if not hyper_lost:
                    continue
                crit = critical_move_index(game, hyper_color)
                if crit is None:
                    continue
                crit["opponent"] = opp
                crit["hyper_color"] = "W" if hyper_color == chess.WHITE else "B"
                losses.append(crit)

    print(f"=== {len(losses)} losses to ground-truth ===")
    print(f"Stockfish at depth {SF_DEPTH}")
    print(f"Path: {STOCKFISH}\n")

    results = []
    with chess.engine.SimpleEngine.popen_uci(STOCKFISH) as sf:
        sf.configure({"Hash": 256, "Threads": 1})
        for i, loss in enumerate(losses, 1):
            try:
                a = analyze_with_sf(sf, loss["fen_before"], loss["move"], SF_DEPTH)
            except Exception as e:
                print(f"  [{i:2d}] {loss['opponent']:<10} mv{loss['fullmove']:3d} "
                      f"{loss['san']:<6} ERROR: {e}")
                continue
            cls = classify(a["best_eval_stm"], a["delta"])
            pc = piece_count(loss["fen_before"])
            results.append({
                **loss,
                **a,
                "classification": cls,
                "piece_count": pc,
            })
            print(f"  [{i:2d}] {loss['opponent']:<10} mv{loss['fullmove']:3d} "
                  f"{loss['san']:<6} | sf_best={a['best_eval_stm']:+5d} "
                  f"sf_actual={a['hyper_eval_stm']:+5d} gap={a['delta']:+5d} "
                  f"pc={pc:2d} -> {cls}")

    # Aggregate
    print("\n=== Classification distribution ===")
    cls_counts = Counter(r["classification"] for r in results)
    for k, v in cls_counts.most_common():
        print(f"  {k:<16} {v:3d}  ({v/len(results)*100:.0f}%)")

    print("\n=== Piece count at critical position ===")
    pc_buckets = Counter()
    for r in results:
        pc = r["piece_count"]
        if pc <= 5:        pc_buckets["1-5  (Syzygy can help)"] += 1
        elif pc <= 7:      pc_buckets["6-7  (Syzygy can't, near-endgame)"] += 1
        elif pc <= 12:     pc_buckets["8-12 (early endgame)"] += 1
        elif pc <= 20:     pc_buckets["13-20 (middlegame)"] += 1
        else:              pc_buckets["21+  (early/middle)"] += 1
    for k in sorted(pc_buckets.keys()):
        print(f"  {k:<32} {pc_buckets[k]:3d}")

    print("\n=== True blunders only — top gaps ===")
    blunders = [r for r in results if r["classification"] == "true-blunder"]
    print(f"  Total true blunders: {len(blunders)}")
    blunders.sort(key=lambda r: -r["delta"])
    for r in blunders[:10]:
        sf_best_uci = r["best_move"].uci() if r["best_move"] else "??"
        print(f"  {r['opponent']:<10} mv{r['fullmove']:3d}  "
              f"played={r['san']:<6} sf_best={sf_best_uci:<6} gap={r['delta']:+5d} pc={r['piece_count']}")

if __name__ == "__main__":
    main()
