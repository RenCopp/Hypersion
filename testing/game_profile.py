"""Game profiling on existing PGN library:
  1. Time-management profiling — how Hypersion spends time across game phases,
     and whether losses correlate with time misallocation.
  2. Opening / position-type weakness — score by ECO family + by structure
     keywords.
"""

import chess
import chess.pgn
import re
import sys
import io
from collections import defaultdict, Counter
from pathlib import Path

PGNS = [
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_sf2400.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_obsidian.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_alexandria.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_rubichess.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet_vs_sf2400.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet_vs_obsidian.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet_vs_alexandria.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet_vs_rubichess.pgn",
    r"C:\Engine\Hypersion\testing\ab_lmrhist_vs_v1.pgn",
    r"C:\Engine\Hypersion\testing\ab_200g_vs_v1.pgn",
]

HYPER_NAMES = {"Hyp_LMR", "Hyp_DEV", "Hyp_V1", "Hyp_DEV", "Hyp_4P", "Hyp_A", "Hyp_A2", "Hyp_CAP"}

COMMENT_RE = re.compile(r'([+\-]?M?\d+(?:\.\d+)?)\s*/\s*(\d+)\s+([\d.]+)s')

def parse_comment(comment):
    """Return (eval_cp, depth, time_sec) from PGN comment, or None."""
    m = COMMENT_RE.search(comment or "")
    if not m:
        return None
    eval_str, depth_str, time_str = m.group(1), m.group(2), m.group(3)
    try:
        depth = int(depth_str)
        time_sec = float(time_str)
    except ValueError:
        return None
    return (eval_str, depth, time_sec)

def game_phase(move_num):
    """Map full-move number to phase label."""
    if move_num <= 12: return "opening"
    if move_num <= 30: return "early-mid"
    if move_num <= 50: return "late-mid"
    if move_num <= 80: return "endgame"
    return "deep-endgame"

def main():
    # Aggregate state
    # phase -> [time_secs] for HYPERSION moves
    time_by_phase = defaultdict(list)
    # outcome -> [total_time_sec_used_by_hypersion_in_game]
    total_time_by_outcome = defaultdict(list)
    # outcome -> phase -> [time_secs]
    time_by_outcome_phase = defaultdict(lambda: defaultdict(list))
    # ECO -> (W, D, L) for hypersion
    eco_wdl = defaultdict(lambda: [0, 0, 0])
    # ECO bucket (first letter) -> WDL
    eco_bucket_wdl = defaultdict(lambda: [0, 0, 0])
    # Per-opponent stats (opening type by who plays it)
    opening_first_moves = Counter()
    open_first_wdl = defaultdict(lambda: [0, 0, 0])

    games_seen = 0
    games_kept = 0
    for pgn_path in PGNS:
        if not Path(pgn_path).exists():
            continue
        with open(pgn_path) as f:
            while True:
                game = chess.pgn.read_game(f)
                if game is None:
                    break
                games_seen += 1
                white = game.headers.get("White", "")
                black = game.headers.get("Black", "")
                # Find which side is Hypersion
                if white in HYPER_NAMES or any(white.startswith(h) for h in HYPER_NAMES):
                    hyper_color = chess.WHITE
                elif black in HYPER_NAMES or any(black.startswith(h) for h in HYPER_NAMES):
                    hyper_color = chess.BLACK
                else:
                    continue
                # If both sides are Hypersion (self-play), only count once: White's perspective
                both_hyp = (white in HYPER_NAMES or any(white.startswith(h) for h in HYPER_NAMES)) and \
                           (black in HYPER_NAMES or any(black.startswith(h) for h in HYPER_NAMES))
                games_kept += 1
                result = game.headers.get("Result", "*")
                eco = game.headers.get("ECO", "?")

                if result == "1-0":
                    outcome = "W" if hyper_color == chess.WHITE else "L"
                elif result == "0-1":
                    outcome = "L" if hyper_color == chess.WHITE else "W"
                elif result == "1/2-1/2":
                    outcome = "D"
                else:
                    outcome = "?"

                if outcome != "?":
                    eco_wdl[eco]["WDL".index(outcome)] += 1
                    eco_bucket_wdl[eco[:1]]["WDL".index(outcome)] += 1

                # Walk the game extracting Hypersion's per-move time
                board = game.board()
                hyper_total_time = 0.0
                for ply, node in enumerate(game.mainline()):
                    side = chess.WHITE if (ply % 2 == 0) else chess.BLACK
                    move = node.move
                    if side == hyper_color:
                        parsed = parse_comment(node.comment)
                        if parsed is not None:
                            _, _, time_sec = parsed
                            full_move = ply // 2 + 1
                            phase = game_phase(full_move)
                            time_by_phase[phase].append(time_sec)
                            if outcome != "?":
                                time_by_outcome_phase[outcome][phase].append(time_sec)
                            hyper_total_time += time_sec
                    # First move tracking (only White's first move = Hypersion-as-White's openings)
                    if ply == 0 and hyper_color == chess.WHITE and outcome != "?":
                        san = board.san(move)
                        opening_first_moves[san] += 1
                        open_first_wdl[san]["WDL".index(outcome)] += 1
                    board.push(move)

                if outcome != "?":
                    total_time_by_outcome[outcome].append(hyper_total_time)

    print(f"=== Games scanned: {games_seen}; with Hypersion: {games_kept} ===\n")

    # ----- 1. Time per phase -----
    print("=== 1. Time-per-move by game phase (seconds) ===")
    print(f"  {'phase':<14} {'count':>6} {'mean':>7} {'median':>7} {'p90':>7} {'p99':>7}")
    for phase in ["opening", "early-mid", "late-mid", "endgame", "deep-endgame"]:
        ts = sorted(time_by_phase[phase])
        if not ts: continue
        n = len(ts)
        mean = sum(ts) / n
        median = ts[n // 2]
        p90 = ts[int(n * 0.9)] if n > 10 else ts[-1]
        p99 = ts[int(n * 0.99)] if n > 100 else ts[-1]
        print(f"  {phase:<14} {n:>6} {mean:>7.3f} {median:>7.3f} {p90:>7.3f} {p99:>7.3f}")

    # ----- 2. Time-per-move in wins vs losses -----
    print("\n=== 2. Time-per-move by phase × outcome (mean seconds) ===")
    print(f"  {'phase':<14} {'W mean':>9} {'D mean':>9} {'L mean':>9}  {'L-W':>8}")
    for phase in ["opening", "early-mid", "late-mid", "endgame", "deep-endgame"]:
        means = {}
        for outcome in "WDL":
            ts = time_by_outcome_phase[outcome][phase]
            means[outcome] = sum(ts) / len(ts) if ts else 0.0
        delta = means["L"] - means["W"]
        print(f"  {phase:<14} {means['W']:>9.3f} {means['D']:>9.3f} {means['L']:>9.3f}  {delta:>+8.3f}")

    # ----- 3. Total time per game by outcome -----
    print("\n=== 3. Total game time (Hypersion side) by outcome ===")
    for outcome in "WDL":
        tts = total_time_by_outcome[outcome]
        if not tts: continue
        n = len(tts)
        mean = sum(tts) / n
        median = sorted(tts)[n // 2]
        print(f"  {outcome}  count={n:>4}  mean={mean:>6.1f}s  median={median:>6.1f}s")

    # ----- 4. ECO bucket WDL -----
    print("\n=== 4. Score by ECO family (Hypersion's perspective) ===")
    print("  bucket             W   D   L  total  score%")
    eco_names = {
        "A": "A: flank/Eng", "B": "B: Sicilian/Caro", "C": "C: e4 e5 / French",
        "D": "D: QGD/queen pawn", "E": "E: KID/closed",
    }
    for bucket in sorted(eco_bucket_wdl.keys()):
        w, d, l = eco_bucket_wdl[bucket]
        total = w + d + l
        if total == 0: continue
        score_pct = (w + 0.5 * d) / total * 100
        name = eco_names.get(bucket, f"{bucket}:")
        print(f"  {name:<18} {w:>3} {d:>3} {l:>3}  {total:>5}  {score_pct:>5.1f}%")

    # ----- 5. Per-ECO weakest top 10 -----
    print("\n=== 5. Bottom-10 ECO codes by score (min 5 games) ===")
    rows = []
    for eco, (w, d, l) in eco_wdl.items():
        total = w + d + l
        if total < 5 or eco == "?": continue
        score_pct = (w + 0.5 * d) / total * 100
        rows.append((score_pct, eco, w, d, l, total))
    rows.sort()
    for score, eco, w, d, l, total in rows[:10]:
        print(f"  {eco:<5}  {w:>2}W/{d:>2}D/{l:>2}L  total={total:>3}  score={score:>5.1f}%")

    # ----- 6. White first-move score (Hypersion as White) -----
    if open_first_wdl:
        print("\n=== 6. White first-move score (Hypersion as White) ===")
        rows = []
        for san, (w, d, l) in open_first_wdl.items():
            total = w + d + l
            if total < 5: continue
            score_pct = (w + 0.5 * d) / total * 100
            rows.append((-total, score_pct, san, w, d, l, total))
        rows.sort()
        for _, score, san, w, d, l, total in rows[:8]:
            print(f"  1.{san:<6}  {w:>2}W/{d:>2}D/{l:>2}L  total={total:>3}  score={score:>5.1f}%")

if __name__ == "__main__":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    main()
