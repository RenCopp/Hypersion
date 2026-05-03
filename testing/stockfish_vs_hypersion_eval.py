"""Compare Hypersion's eval to Stockfish's eval at the SAME depth, on the
same positions where divergence was detected. This isolates whether the gap
we saw is:
  (a) depth — Hypersion at game-TC depth 12 vs SF analysis at depth 16
  (b) eval port — Hypersion's NNUE inference produces different values
"""

import chess
import chess.engine
import chess.pgn
import re
from pathlib import Path

# Same positions where divergence was detected — extract by re-running the
# walk but only collecting positions, not analyzing.
PGNS = [
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_obsidian.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_sf2400.pgn",
]
HYPER_NAME = "Hyp_LMR"
STOCKFISH = r"C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe"
HYPERSION = r"C:\Engine\Hypersion\Hypersion.exe"
DEPTH = 16
MAX_POSITIONS = 8  # quick sample

EVAL_RE = re.compile(r'^([+\-]?)(?:M(\d+)|(\d+(?:\.\d+)?))$')

def parse_eval(s):
    m = EVAL_RE.match(s)
    if not m: return None
    sign_str, mate_str, num_str = m.groups()
    sign = -1 if sign_str == '-' else 1
    if mate_str is not None: return sign * 1500
    return int(round(sign * float(num_str) * 100))

def collect_one_loss_position(pgn_path):
    """Return one (board, hyper_eval) pair from the FIRST loss in this PGN
    where Hypersion was overoptimistic by 100+ cp at depth 12."""
    out = []
    with open(pgn_path) as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None: break
            white = game.headers.get("White", "")
            black = game.headers.get("Black", "")
            result = game.headers.get("Result", "*")
            if HYPER_NAME == white:   hyper_color = chess.WHITE
            elif HYPER_NAME == black: hyper_color = chess.BLACK
            else: continue
            hyper_lost = (result == "0-1" and hyper_color == chess.WHITE) or \
                         (result == "1-0" and hyper_color == chess.BLACK)
            if not hyper_lost: continue

            board = game.board()
            for ply, node in enumerate(game.mainline()):
                side = chess.WHITE if (ply % 2 == 0) else chess.BLACK
                if side == hyper_color:
                    comment = node.comment or ""
                    mc = re.match(r'\s*([+\-]?M?\d+(?:\.\d+)?)\s*/\s*(\d+)', comment)
                    if mc:
                        h_eval = parse_eval(mc.group(1))
                        if h_eval is not None and abs(h_eval) < 1500:
                            # Take this position — fullmove between 8 and 25
                            fullmove = ply // 2 + 1
                            if 8 <= fullmove <= 25:
                                out.append((board.copy(), h_eval, fullmove))
                                if len(out) >= MAX_POSITIONS:
                                    return out
                board.push(node.move)
    return out

def get_eval(engine, board, depth):
    info = engine.analyse(board, chess.engine.Limit(depth=depth))
    score = info["score"].white()
    stm_sign = 1 if board.turn == chess.WHITE else -1
    return stm_sign * (score.score(mate_score=15000) or 0)

def main():
    print(f"Comparing Hypersion @ depth {DEPTH} vs Stockfish @ depth {DEPTH}")
    print(f"Goal: see if eval gap survives depth match.\n")

    # Collect a sample of positions
    positions = []
    for pgn in PGNS:
        positions.extend(collect_one_loss_position(pgn)[:4])
    if len(positions) > MAX_POSITIONS:
        positions = positions[:MAX_POSITIONS]
    print(f"Sample size: {len(positions)} positions\n")

    with chess.engine.SimpleEngine.popen_uci(STOCKFISH) as sf, \
         chess.engine.SimpleEngine.popen_uci(HYPERSION) as hyp:
        sf.configure({"Hash": 256, "Threads": 1})
        hyp.configure({"Hash": 256, "Threads": 1})
        print(f"  {'mv':<4} {'in-game':<8} {'hyp@d'+str(DEPTH):<10} {'sf@d'+str(DEPTH):<10} "
              f"{'h-sf':<6} {'depth-only?':<12}")
        for board, h_in_game, fullmove in positions:
            try:
                h_d = get_eval(hyp, board, DEPTH)
                s_d = get_eval(sf,  board, DEPTH)
            except Exception as e:
                print(f"  ERROR: {e}")
                continue
            in_game_to_sf = h_in_game - s_d   # original gap
            depth_match_to_sf = h_d - s_d       # gap when both at same depth
            note = "depth only" if abs(depth_match_to_sf) < 50 else "real eval gap"
            print(f"  {fullmove:<4} {h_in_game:+5d}    {h_d:+5d}      {s_d:+5d}      "
                  f"{depth_match_to_sf:+5d}  {note}")

if __name__ == "__main__":
    main()
