#!/usr/bin/env python3
"""
Hypersion Lichess bot.

Reads the API token from C:\\Engine\\Hypersion\\Hypersion.txt and connects the
Hypersion UCI engine to the Lichess Bot API. Designed to be lean: one process
per game, spawns Hypersion.exe via subprocess, talks UCI over stdin/stdout,
and shovels moves through Lichess's `bot/game/stream` event loop.

Tested against Lichess BOT API as of 2026.

Usage:
    python lichess_bot.py [--engine PATH] [--token-file PATH]

Default engine:     ../Hypersion.exe (relative to this script)
Default token file: ../Hypersion.txt (relative to this script)

Requires:
    pip install berserk
"""

import argparse
import logging
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    import berserk
except ImportError:
    print("ERROR: 'berserk' not installed. Run: pip install berserk", file=sys.stderr)
    sys.exit(1)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("hypersion-bot")


# ---------------------------------------------------------------------------
# Engine wrapper
# ---------------------------------------------------------------------------
class UCIEngine:
    """Minimal UCI driver — just enough for Lichess play."""

    def __init__(self, path: str, options: dict = None):
        self.path = path
        self.options = options or {}
        self.proc = None
        self._lock = threading.Lock()

    def start(self):
        log.info(f"Starting engine: {self.path}")
        self.proc = subprocess.Popen(
            [self.path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=os.path.dirname(os.path.abspath(self.path)),
            text=True,
            bufsize=1,
            universal_newlines=True,
        )
        self._send("uci")
        self._wait_for("uciok")
        for name, value in self.options.items():
            self._send(f"setoption name {name} value {value}")
        self._send("isready")
        self._wait_for("readyok")
        log.info("Engine ready.")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                self._send("quit")
                self.proc.wait(timeout=2)
            except Exception:
                self.proc.kill()
        self.proc = None

    def is_alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def restart(self):
        """Kill+respawn the subprocess. Used after crash detection."""
        log.warning("Engine restart requested")
        self.stop()
        self.start()

    def set_option(self, name: str, value):
        """Live setoption between games. Skips if engine is dead."""
        if not self.is_alive():
            return
        with self._lock:
            self._send(f"setoption name {name} value {value}")

    def set_strength(self, target_elo: int = None):
        """Cap engine strength to a target ELO. Pass None to clear (full strength).
        Internally toggles UCI_LimitStrength + UCI_Elo. Engine maps the ELO
        through src/search.cpp:283 — 500→skill 0 (very weak), 1500→skill 8
        (~club), 2500→skill 16, 3200→full. NNUE remains loaded; strength is
        capped via depth + move noise, not by disabling NNUE."""
        if target_elo is None or target_elo >= 3200:
            self.set_option("UCI_LimitStrength", "false")
            log.info("Engine strength: FULL")
        else:
            clamped = max(500, min(3200, int(target_elo)))
            self.set_option("UCI_LimitStrength", "true")
            self.set_option("UCI_Elo", clamped)
            log.info(f"Engine strength capped to ~{clamped} Elo")

    def new_game(self):
        with self._lock:
            self._send("ucinewgame")
            self._send("isready")
            self._wait_for("readyok")

    def go(self, position_moves: list, wtime: int, btime: int, winc: int, binc: int):
        """Tell the engine to think; return (uci_move, last_score_cp, last_mate)."""
        with self._lock:
            self._send(f"position startpos moves {' '.join(position_moves)}")
            self._send(
                f"go wtime {wtime} btime {btime} winc {winc} binc {binc}"
            )
            return self._wait_for_bestmove_with_score()

    # ---- Internals ----
    def _send(self, line: str):
        log.debug(f">> {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _wait_for(self, token: str, timeout: float = 10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline().strip()
            if not line:
                continue
            log.debug(f"<< {line}")
            if line == token or line.startswith(token + " "):
                return line
        raise TimeoutError(f"engine never sent {token!r}")

    def _wait_for_bestmove(self) -> str:
        mv, _, _ = self._wait_for_bestmove_with_score()
        return mv

    def _wait_for_bestmove_with_score(self):
        last_cp = None
        last_mate = None
        while True:
            line = self.proc.stdout.readline().strip()
            if not line:
                if self.proc.poll() is not None:
                    raise RuntimeError("engine died mid-search")
                continue
            log.debug(f"<< {line}")
            if line.startswith("info ") and " score " in line:
                # Capture the most recent score for resign / draw decisions.
                parts = line.split()
                try:
                    si = parts.index("score")
                    kind = parts[si + 1]
                    val  = int(parts[si + 2])
                    if kind == "cp":   last_cp,   last_mate = val, None
                    elif kind == "mate": last_cp, last_mate = None, val
                except (ValueError, IndexError):
                    pass
            if line.startswith("bestmove "):
                tokens = line.split()
                return (tokens[1] if len(tokens) > 1 else "0000", last_cp, last_mate)


# ---------------------------------------------------------------------------
# Lichess game handler
# ---------------------------------------------------------------------------
class BotStats:
    """Tiny win/loss/draw tracker so we have something to print on shutdown."""
    def __init__(self):
        self.wins = 0
        self.losses = 0
        self.draws = 0
        self._lock = threading.Lock()
    def record(self, result_for_us: str):
        with self._lock:
            if result_for_us == "win":   self.wins   += 1
            elif result_for_us == "loss": self.losses += 1
            elif result_for_us == "draw": self.draws  += 1
    def summary(self) -> str:
        total = self.wins + self.losses + self.draws
        if total == 0: return "no games played"
        score = self.wins + self.draws / 2.0
        return f"{self.wins}W / {self.draws}D / {self.losses}L ({score}/{total})"


class GameHandler(threading.Thread):
    def __init__(self, client: berserk.Client, engine: UCIEngine, game_id: str, our_id: str, stats: BotStats = None, enable_chat: bool = True):
        super().__init__(daemon=True)
        self.client = client
        self.engine = engine
        self.game_id = game_id
        self.our_id = our_id
        self.stats = stats
        self.enable_chat = enable_chat
        self._said_hello = False

    def _chat(self, text: str):
        if not self.enable_chat:
            return
        try:
            # spectator=False → opponent sees the message in the game chat.
            self.client.bots.post_message(self.game_id, text, spectator=False)
        except Exception as e:
            log.debug(f"chat send failed: {e}")

    def _decide_strength(self, opp: dict, tc_kind: str) -> int:
        """Given the opponent player block from a `gameFull` event, decide what
        Elo to cap our engine at for this game. Returns None for full strength.

        Heuristics (in order):
        - Bots (title=BOT) → full strength. The user wants AI sparring honest.
        - Provisional rating (lichess `provisional: true`, often new accounts /
          smurfs) → use max(rating, 1700). New player ratings are noisy; many
          alts are actually strong players whose first 30 games are unrated.
        - Rated >= 2500 → full strength (we're being challenged).
        - Casual / no rating → 1500 baseline.
        - Otherwise → match the opponent's rating directly.

        Bullet TC stays slightly stronger because making weak moves
        believable in 1-second thinks is hard — under-cap makes us look
        glitchy rather than bad.
        """
        title = (opp.get("title") or "").upper()
        if title == "BOT":
            return None  # full strength vs bots
        rating = opp.get("rating")
        provisional = opp.get("provisional", False)
        if rating is None:
            return 1500
        if provisional:
            return max(int(rating), 1700)
        if rating >= 2500:
            return None
        target = int(rating)
        # Bullet adjustment: small floor so weak-move-noise has time to register.
        if tc_kind == "bullet":
            target = max(target, 1100)
        return target

    def run(self):
        log.info(f"[game {self.game_id}] starting")
        try:
            self.engine.new_game()
            stream = self.client.bots.stream_game_state(self.game_id)
            our_color = None
            for event in stream:
                etype = event.get("type", "")
                if etype == "gameFull":
                    our_color = "white" if event["white"]["id"] == self.our_id else "black"
                    opp = event["black"] if our_color == "white" else event["white"]
                    tc_kind = (event.get("speed") or "rapid").lower()
                    target = self._decide_strength(opp, tc_kind)
                    log.info(
                        f"[game {self.game_id}] vs {opp.get('name','?')} "
                        f"(rating={opp.get('rating','?')} "
                        f"prov={opp.get('provisional', False)} "
                        f"title={opp.get('title','-')}) "
                        f"speed={tc_kind} → target_elo={target}"
                    )
                    self.engine.set_strength(target)
                    if not self._said_hello:
                        self._chat("Hi, glhf!")
                        self._said_hello = True
                    state = event["state"]
                elif etype == "gameState":
                    state = event
                elif etype == "chatLine":
                    continue
                else:
                    continue

                status = state.get("status", "started")
                if status not in ("created", "started"):
                    winner = state.get("winner")        # "white" / "black" / None
                    # Stats + sign-off chat.
                    if self.stats is not None:
                        if winner is None:
                            self.stats.record("draw")
                        elif winner == our_color:
                            self.stats.record("win")
                        else:
                            self.stats.record("loss")
                    if winner is None:
                        self._chat("gg")
                    elif winner == our_color:
                        self._chat("gg, well played!")
                    else:
                        self._chat("gg, nice game!")
                    log.info(f"[game {self.game_id}] game ended: {status}")
                    break

                moves = state.get("moves", "").split() if state.get("moves") else []
                whose_turn = "white" if len(moves) % 2 == 0 else "black"
                if whose_turn != our_color:
                    continue

                wtime = state.get("wtime", 60_000)
                btime = state.get("btime", 60_000)
                winc  = state.get("winc", 0)
                binc  = state.get("binc", 0)

                # Lichess sometimes reports time as datetime.timedelta — normalize.
                def ms(x):
                    if hasattr(x, "total_seconds"):
                        return int(x.total_seconds() * 1000)
                    return int(x)
                wtime, btime = ms(wtime), ms(btime)
                winc, binc = ms(winc), ms(binc)

                move, score_cp, score_mate = self.engine.go(moves, wtime, btime, winc, binc)
                if move == "0000":
                    log.warning(f"[game {self.game_id}] engine had no move")
                    break

                # ---- Resign / draw decisions ----
                # Track recent eval — resign only after several moves of a clearly
                # lost position to avoid surrendering on a single noisy iteration.
                if not hasattr(self, "_lost_streak"):
                    self._lost_streak = 0
                if score_mate is not None and score_mate < 0:
                    self._lost_streak += 1
                elif score_cp is not None and score_cp < -900:
                    self._lost_streak += 1
                else:
                    self._lost_streak = 0
                # Resign once we've seen 5 consecutive moves of "completely lost".
                if self._lost_streak >= 5 and len(moves) > 30:
                    log.info(f"[game {self.game_id}] resigning (streak={self._lost_streak})")
                    try: self.client.bots.resign_game(self.game_id)
                    except Exception: pass
                    break

                # Accept draw offers in clearly drawn or worse positions.
                if state.get("wdraw") or state.get("bdraw"):
                    if score_cp is not None and abs(score_cp) < 25:
                        try: self.client.bots.accept_draw(self.game_id)
                        except Exception: pass

                log.info(f"[game {self.game_id}] our move: {move} (cp={score_cp} mate={score_mate})")
                try:
                    self.client.bots.make_move(self.game_id, move)
                except berserk.exceptions.ResponseError as e:
                    log.error(f"[game {self.game_id}] make_move failed: {e}")
                    break
        except Exception as e:
            log.exception(f"[game {self.game_id}] handler crashed: {e}")
            # Best-effort: try to resign so we don't time out and waste
            # opponent time. If resign fails (network, etc.) lichess will
            # eventually time us out; the auto-restart at next gameStart
            # will at least bring the engine back for future games.
            try:
                self.client.bots.resign_game(self.game_id)
            except Exception:
                pass
        finally:
            # Reset engine strength so the next game's _decide_strength()
            # always starts from a known baseline.
            try:
                self.engine.set_strength(None)
            except Exception:
                pass
        log.info(f"[game {self.game_id}] handler exit")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--engine",
        default=str(Path(__file__).resolve().parent.parent / "Hypersion.exe"),
        help="Path to Hypersion.exe",
    )
    parser.add_argument(
        "--token-file",
        default=str(Path(__file__).resolve().parent.parent / "Hypersion.txt"),
        help="Lichess BOT API token file (default: ../Hypersion.txt)",
    )
    parser.add_argument("--hash", type=int, default=128, help="Hash MB")
    parser.add_argument("--threads", type=int, default=1, help="Threads")
    parser.add_argument("--accept-rated", action="store_true", default=False,
                        help="Accept rated challenges (default: casual only)")
    parser.add_argument("--max-games", type=int, default=2,
                        help="Max simultaneous games")
    parser.add_argument(
        "--max-tc-min",
        type=int,
        default=0,
        help="Reject games with initial TC below this many minutes (0 = accept all)",
    )
    parser.add_argument(
        "--log-file",
        default=None,
        help="Mirror logs to this file in addition to stdout",
    )
    parser.add_argument(
        "--no-chat",
        action="store_true",
        default=False,
        help="Disable game-start hello + game-end thanks chat",
    )
    args = parser.parse_args()

    if args.log_file:
        fh = logging.FileHandler(args.log_file, encoding="utf-8")
        fh.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s",
                                          datefmt="%Y-%m-%d %H:%M:%S"))
        log.addHandler(fh)
        log.info(f"mirroring logs to {args.log_file}")

    # Load token.
    token_path = Path(args.token_file)
    if not token_path.is_file():
        log.error(f"Token file not found: {token_path}")
        sys.exit(1)
    token = token_path.read_text(encoding="utf-8").strip()
    if not token:
        log.error("Token file is empty")
        sys.exit(1)

    # Connect to Lichess.
    session = berserk.TokenSession(token)
    client = berserk.Client(session=session)
    me = client.account.get()
    our_id = me["id"]
    log.info(f"Logged in as {me['username']} (bot={me.get('title')=='BOT'})")
    if me.get("title") != "BOT":
        log.warning(
            "Account is not a BOT. Run `curl -d '' https://lichess.org/api/bot/account/upgrade -H \"Authorization: Bearer <TOKEN>\"` first."
        )

    # Boot engine.
    engine = UCIEngine(args.engine, options={"Hash": args.hash, "Threads": args.threads})
    engine.start()

    stats = BotStats()
    active = {}
    try:
        for event in client.bots.stream_incoming_events():
            etype = event.get("type", "")
            if etype == "challenge":
                ch = event["challenge"]
                tc = ch.get("timeControl", {})
                tc_kind = tc.get("type", "unlimited")
                rated = ch.get("rated", False)
                variant = ch.get("variant", {}).get("key", "standard")
                ch_id = ch["id"]
                reason = None
                if variant != "standard":
                    reason = "non-standard variant"
                elif tc_kind == "unlimited":
                    reason = "unlimited time"
                elif rated and not args.accept_rated:
                    reason = "rated games disabled"
                elif args.max_tc_min and tc.get("limit", 0) // 60 < args.max_tc_min:
                    reason = "time control too short"
                elif len(active) >= args.max_games:
                    reason = "already at max concurrent games"
                if reason:
                    log.info(f"declining {ch_id}: {reason}")
                    try:
                        client.bots.decline_challenge(ch_id, reason="generic")
                    except Exception:
                        pass
                else:
                    log.info(f"accepting {ch_id} ({tc.get('show', '?')} {variant})")
                    try:
                        client.bots.accept_challenge(ch_id)
                    except Exception as e:
                        log.warning(f"accept failed: {e}")
            elif etype == "gameStart":
                gid = event["game"]["gameId"]
                if gid in active:
                    continue
                # If the engine died (e.g. a previous game crashed it), bring
                # it back up before handing the new game over.
                if not engine.is_alive():
                    log.warning("engine subprocess is dead; restarting before new game")
                    try:
                        engine.restart()
                    except Exception as e:
                        log.exception(f"engine restart failed: {e}")
                        try:
                            client.bots.abort_game(gid)
                        except Exception:
                            pass
                        continue
                handler = GameHandler(
                    client, engine, gid, our_id,
                    stats=stats,
                    enable_chat=not args.no_chat,
                )
                active[gid] = handler
                handler.start()
            elif etype == "gameFinish":
                gid = event["game"]["gameId"]
                active.pop(gid, None)
                log.info(f"finished game {gid}")
    except KeyboardInterrupt:
        log.info("shutting down (Ctrl+C)")
    finally:
        log.info(f"session stats: {stats.summary()}")
        engine.stop()


if __name__ == "__main__":
    main()
