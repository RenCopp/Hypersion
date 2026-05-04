"""Stockfish-driven analysis of every Hypersion move in the 50g match.

For each Hypersion move:
  1. Stockfish analyses the position BEFORE the move (depth 16, multipv=1)
     to get the optimal move + eval (E_best)
  2. Stockfish analyses the position AFTER Hypersion's move (depth 16) to
     get the eval after the move (E_actual, from Hypersion's POV via negate)
  3. Eval delta = E_best - E_actual = "how many cp Hypersion gave up"

A move is flagged as:
  - blunder    : delta >= 200 cp
  - mistake    : delta 100..199 cp
  - inaccuracy : delta 50..99 cp
  - ok         : delta < 50 cp

Aggregates across all games:
  - Distribution by game phase (opening/MG/EG by move#)
  - Distribution by piece type of Hypersion's actual move
  - Distribution by piece type of Stockfish's recommended move
  - Top 10 worst blunders with FENs for inspection
"""

import chess
import chess.engine
import chess.pgn
import time
from collections import Counter, defaultdict
from pathlib import Path

PGN_PATH    = r"C:\Engine\Hypersion\testing\match_50g_vs_sf.pgn"
STOCKFISH   = r"C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe"
SF_DEPTH    = 14   # depth 14 is fast and reliable for Hyp-level mistakes
HYPER_NAME  = "Hypersion"

def piece_letter(san):
    """First non-coord char in SAN gives the piece (K Q R B N) or 'P' for pawn / 'O' for castle."""
    if san.startswith("O-O"): return "O"
    return san[0] if san[0] in "KQRBN" else "P"

def game_phase(move_no):
    if move_no <= 12:  return "opening"
    if move_no <= 28:  return "middlegame"
    if move_no <= 50:  return "early-end"
    return "deep-end"

def main():
    games = []
    with open(PGN_PATH) as f:
        while True:
            g = chess.pgn.read_game(f)
            if g is None: break
            games.append(g)
    print(f"Read {len(games)} games from {PGN_PATH}")

    # Aggregates
    blunder_count = 0
    mistake_count = 0
    inaccuracy_count = 0
    move_total = 0

    by_phase = defaultdict(lambda: defaultdict(int))   # phase -> category -> count
    by_played_piece = defaultdict(lambda: defaultdict(int))
    by_recommended_piece = defaultdict(lambda: defaultdict(int))
    blunder_records = []   # for top-10 listing

    sf_total_calls = 0
    t_start = time.time()

    with chess.engine.SimpleEngine.popen_uci(STOCKFISH) as sf:
        sf.configure({"Hash": 256, "Threads": 1})

        for gi, game in enumerate(games, 1):
            white = game.headers.get("White", "")
            black = game.headers.get("Black", "")
            result = game.headers.get("Result", "*")
            if HYPER_NAME == white:   hyper_color = chess.WHITE
            elif HYPER_NAME == black: hyper_color = chess.BLACK
            else: continue

            board = game.board()
            game_blunders = 0
            for ply, node in enumerate(game.mainline()):
                side = chess.WHITE if (ply % 2 == 0) else chess.BLACK
                hyper_move = node.move

                if side == hyper_color:
                    # Position BEFORE Hypersion's move
                    info_best = sf.analyse(board, chess.engine.Limit(depth=SF_DEPTH))
                    sf_total_calls += 1
                    score_best = info_best["score"].pov(hyper_color)
                    e_best = score_best.score(mate_score=15000) or 0
                    best_move = info_best.get("pv", [None])[0]
                    best_san = board.san(best_move) if best_move else "?"
                    played_san = board.san(hyper_move)

                    # Hypersion's actual move
                    board.push(hyper_move)
                    info_after = sf.analyse(board, chess.engine.Limit(depth=SF_DEPTH))
                    sf_total_calls += 1
                    score_after = info_after["score"].pov(hyper_color)
                    e_after = score_after.score(mate_score=15000) or 0

                    delta = e_best - e_after
                    move_no = ply // 2 + 1
                    phase = game_phase(move_no)
                    played_p = piece_letter(played_san)
                    rec_p = piece_letter(best_san) if best_move else "?"

                    if delta >= 200:
                        category = "blunder"
                        blunder_count += 1
                        game_blunders += 1
                        blunder_records.append({
                            "game": gi, "move": move_no, "color": "W" if hyper_color == chess.WHITE else "B",
                            "played": played_san, "rec": best_san,
                            "e_best": e_best, "e_after": e_after, "delta": delta,
                            "fen": chess.Board.fen(board.copy(stack=False)),  # post-move FEN
                            "phase": phase, "piece": played_p,
                        })
                    elif delta >= 100:
                        category = "mistake"
                        mistake_count += 1
                    elif delta >= 50:
                        category = "inaccuracy"
                        inaccuracy_count += 1
                    else:
                        category = "ok"

                    by_phase[phase][category] += 1
                    by_played_piece[played_p][category] += 1
                    by_recommended_piece[rec_p][category] += 1
                    move_total += 1
                else:
                    board.push(hyper_move)

            if gi % 10 == 0:
                elapsed = time.time() - t_start
                print(f"  ... {gi}/{len(games)} games done "
                      f"({sf_total_calls} SF calls, {elapsed:.0f}s)")

    elapsed = time.time() - t_start
    print(f"\n=== Done in {elapsed:.0f}s, {sf_total_calls} SF calls ===\n")

    print(f"Total Hyp moves analyzed: {move_total}")
    print(f"  blunders   (>= 200 cp): {blunder_count} ({blunder_count*100/max(1,move_total):.1f}%)")
    print(f"  mistakes  (100..199):   {mistake_count} ({mistake_count*100/max(1,move_total):.1f}%)")
    print(f"  inaccur.  ( 50..99):    {inaccuracy_count} ({inaccuracy_count*100/max(1,move_total):.1f}%)")
    print(f"  ok        (< 50 cp):    {move_total - blunder_count - mistake_count - inaccuracy_count}")

    print("\n=== Distribution by game phase ===")
    print(f"  {'phase':<14} {'blunder':>8} {'mistake':>8} {'inaccur':>8} {'ok':>8}")
    for phase in ["opening", "middlegame", "early-end", "deep-end"]:
        d = by_phase[phase]
        print(f"  {phase:<14} {d['blunder']:>8} {d['mistake']:>8} {d['inaccuracy']:>8} {d['ok']:>8}")

    print("\n=== Distribution by piece played (when blunder/mistake) ===")
    print(f"  {'piece':<6} {'blunder':>8} {'mistake':>8} {'inaccur':>8} {'ok':>8}")
    pieces_order = ["K", "Q", "R", "B", "N", "P", "O"]
    for p in pieces_order:
        d = by_played_piece[p]
        if not d: continue
        print(f"  {p:<6} {d['blunder']:>8} {d['mistake']:>8} {d['inaccuracy']:>8} {d['ok']:>8}")

    print("\n=== Distribution by piece SF recommended (instead) ===")
    print(f"  {'piece':<6} {'blunder':>8} {'mistake':>8} {'inaccur':>8}")
    for p in pieces_order:
        d = by_recommended_piece[p]
        b, m, i = d['blunder'], d['mistake'], d['inaccuracy']
        if b + m + i == 0: continue
        print(f"  {p:<6} {b:>8} {m:>8} {i:>8}")

    print("\n=== Top 15 worst blunders ===")
    blunder_records.sort(key=lambda r: -r["delta"])
    for r in blunder_records[:15]:
        print(f"  game {r['game']:>2} mv {r['move']:>3}{r['color']}: played {r['played']:<7} "
              f"sf_rec {r['rec']:<7} delta={r['delta']:>5} cp  ({r['phase']})")

if __name__ == "__main__":
    import sys, io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    main()
