"""NPS profiling — measure Hypersion's nodes-per-second across position
categories to detect where search slows down (cache effects, memory
pressure, branching factor explosions, NNUE inference cost variance).

Test positions:
  - Standard opening positions (popular structures)
  - Tactical middlegame (lots of pieces, high branching)
  - Closed positions (lots of pieces, few legal moves)
  - Endgame (few pieces, simple)
  - Pawn endgame (very few pieces)
  - Rook+minor endgame
  - Complex tactical positions (forced sequences)
"""

import chess
import chess.engine
import time
from pathlib import Path

HYPERSION = r"C:\Engine\Hypersion\Hypersion.exe"
DEPTH = 14   # consistent depth so node count varies but we measure NPS

POSITIONS = [
    ("startpos",                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
    ("opening_1.e4",            "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"),
    ("opening_post-e4-c5",      "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"),
    ("opening_post-d4-Nf6",     "rnbqkb1r/pppppppp/5n2/8/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 1 2"),
    ("complex_middlegame",      "r2qkbnr/pp2pppp/2n5/3p1b2/3P4/2N1PN2/PPP2PPP/R1BQKB1R w KQkq - 0 6"),
    ("KID_typical",             "r1bq1rk1/ppp1npbp/2n3p1/3p4/2PP4/2N1PN2/PP3PPP/R1BQ1RK1 w - - 4 8"),
    ("Scotch_C45_main",         "r1bqk2r/pppp1ppp/2n5/4P3/1bP5/2N5/PP3PPP/R1BQKB1R b KQkq - 0 7"),
    ("Najdorf_typical",         "rnbqkb1r/1p2pppp/p2p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w KQkq - 0 6"),
    ("closed_ruy_lopez",        "r1bqk2r/2pp1ppp/p1n2n2/1pb1p3/B3P3/3P1N2/PPP2PPP/RNBQ1RK1 w kq - 0 7"),
    ("tactical_kingside",       "r1b2rk1/pp2qppp/3pn3/2p1pP2/2B1P3/3P3Q/PPP3PP/R1B1K1NR w KQ - 1 11"),
    ("queen_endgame",           "8/4kp2/3p4/3P1q2/8/4Q3/5PK1/8 w - - 0 1"),
    ("rook_endgame",            "8/4k3/3p4/3P4/8/4K3/8/4R3 w - - 0 1"),
    ("rook_minor_endgame",      "8/4kn2/3p4/3P4/8/4KB2/8/4R3 w - - 0 1"),
    ("pawn_endgame",            "8/4k3/3p4/3P4/8/4K3/8/8 w - - 0 1"),
    ("KBN_v_K_mate",            "8/8/8/8/8/2K5/B7/N3k3 w - - 0 1"),
    ("KQ_v_K_winning",          "8/8/8/8/3k4/8/3KQ3/8 w - - 0 1"),
    ("starts_complex_endgame",  "4r1k1/5pbp/2pp1np1/p7/PnB5/3PB1P1/1Pq2P1P/3R2QK b - - 0 1"),
]

def main():
    print(f"NPS profile @ depth {DEPTH} (Hash=64MB, Threads=1)\n")
    print(f"  {'position':<30} {'nodes':>10} {'time_ms':>8} {'NPS':>10} {'notes':<20}")

    results = []
    crashed = []
    for name, fen in POSITIONS:
        board = chess.Board(fen)
        piece_count = chess.popcount(board.occupied)
        try:
            with chess.engine.SimpleEngine.popen_uci(HYPERSION) as eng:
                eng.configure({"Hash": 64, "Threads": 1, "OwnBook": False})
                t0 = time.perf_counter()
                info = eng.analyse(board, chess.engine.Limit(depth=DEPTH))
                t1 = time.perf_counter()
                nodes = info.get("nodes", 0)
                elapsed_ms = (t1 - t0) * 1000
                nps = (nodes / (t1 - t0)) if t1 > t0 else 0
                results.append({
                    "name": name, "nodes": nodes, "elapsed_ms": elapsed_ms,
                    "nps": nps, "piece_count": piece_count
                })
                note = f"pc={piece_count}"
                print(f"  {name:<30} {nodes:>10,} {elapsed_ms:>8.0f} {nps:>10,.0f} {note:<20}")
        except chess.engine.EngineTerminatedError as e:
            print(f"  {name:<30} *** CRASHED *** pc={piece_count}  fen='{fen}'")
            crashed.append({"name": name, "fen": fen, "piece_count": piece_count})

    if crashed:
        print(f"\n!!! CRASHES DETECTED in {len(crashed)} positions:")
        for c in crashed:
            print(f"   - {c['name']}  pc={c['piece_count']}  fen='{c['fen']}'")

    # Aggregate
    print(f"\n=== Aggregate ===")
    if results:
        npss = [r["nps"] for r in results]
        npss.sort()
        n = len(npss)
        print(f"  Min NPS:     {npss[0]:>10,.0f}  ({results[next(i for i,r in enumerate(results) if r['nps']==npss[0])]['name']})")
        print(f"  Median NPS:  {npss[n//2]:>10,.0f}")
        print(f"  Max NPS:     {npss[-1]:>10,.0f}  ({results[next(i for i,r in enumerate(results) if r['nps']==npss[-1])]['name']})")
        print(f"  Mean NPS:    {sum(npss)/n:>10,.0f}")
        spread = npss[-1] / npss[0] if npss[0] > 0 else 0
        print(f"  Spread (max/min): {spread:.2f}x")

    # Categorize by piece count
    print(f"\n=== NPS by piece count ===")
    by_pc = {}
    for r in results:
        bucket = "endgame(<=10)" if r["piece_count"] <= 10 else \
                 "middlegame(11-22)" if r["piece_count"] <= 22 else \
                 "opening(23+)"
        by_pc.setdefault(bucket, []).append(r["nps"])
    for k in sorted(by_pc.keys()):
        npss = by_pc[k]
        avg = sum(npss) / len(npss)
        print(f"  {k:<22}  count={len(npss):>2}  mean NPS = {avg:,.0f}")

if __name__ == "__main__":
    main()
