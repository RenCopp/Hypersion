# Hypersion Lichess Bot

Connects the Hypersion UCI engine to Lichess via the BOT API.
Adaptive strength: matches the engine's level to the opponent's rating.

## Quick start

1. **Get an API token** at <https://lichess.org/account/oauth/token/create>
   with the `bot:play` scope.
2. **Save it** as `Hypersion.txt` in the project root (one line, no quotes — the same directory that contains `Hypersion.exe`).
3. **Upgrade the account** to a bot account (one-time, requires a fresh
   account with no rated games):
   ```
   curl -d "" https://lichess.org/api/bot/account/upgrade -H "Authorization: Bearer <TOKEN>"
   ```
4. **Run** `run.bat`. First run installs the venv and `berserk`.

## Adaptive strength

When a game starts, the bot reads the opponent's rating from the
`gameFull` event and decides what strength to play at:

| Opponent | Target strength |
|---|---|
| Bot (`title=BOT`) | full strength |
| Provisional rating | max(rating, 1700) — new accounts are noisy |
| Rated ≥ 2500 | full strength |
| Rated < 2500 | match opponent's rating |
| Casual / unrated | 1500 baseline |
| Bullet TC | floor of 1100 (low caps look glitchy at 1s/move) |

The strength cap is implemented engine-side via UCI `UCI_LimitStrength`
+ `UCI_Elo`. Hypersion maps that to a depth cap + move-selection noise
(see `src/search.cpp:283`). NNUE stays loaded — we don't disable the
network just because we're playing weaker.

## Command-line flags

```
python lichess_bot.py [options]

  --engine PATH        path to Hypersion.exe (default: ../Hypersion.exe)
  --token-file PATH    API token file (default: ../Hypersion.txt)
  --hash MB            engine hash table (default: 128)
  --threads N          UCI Threads option (default: 1; Lazy SMP buggy, leave 1)
  --accept-rated       accept rated challenges (default: casual only)
  --max-games N        simultaneous games cap (default: 2)
  --max-tc-min N       reject TC below this many minutes (0 = accept all)
  --log-file PATH      mirror logs to a file
  --no-chat            disable game-start hello + game-end thanks chat
```

`run.bat` already wires sensible defaults: `--hash 256 --threads 1
--max-games 2`. Edit it to taste.

## What the bot does

- **Streams events** from `bot/stream/event` and per-game `bot/game/stream/{id}`.
- **Filters challenges**: declines variants, unlimited TC, rated games
  (unless `--accept-rated`), TC below `--max-tc-min`, and challenges that
  would exceed `--max-games`.
- **Adapts strength** per game, resets between games.
- **Plays one move per `gameState`** event when it's our turn, using the
  remaining clock from the event.
- **Resigns** after 5 consecutive iterations of evaluation < −9 pawns OR
  any negative mate-distance, but only past move 30 to avoid resigning
  noisy openings.
- **Accepts draw offers** when our eval magnitude is < 25 cp (close to a
  drawn position).
- **Posts chat**: `Hi, glhf!` at start, `gg, ...` at end (suppress with `--no-chat`).
- **Auto-restarts the engine** if its subprocess dies between games.
- **Tracks W/D/L** for the session, prints on shutdown.

## Files

```
lichess_bot/
├── lichess_bot.py     ← main bot script
├── run.bat            ← Windows launcher; creates venv + runs the bot
├── requirements.txt   ← berserk only
├── README.md          ← this file
└── PROFILE.md         ← copy-pasteable bio / setup content
```

## Running on a server

Same script, but on Linux you'll want `tmux` or `systemd`:

```ini
# /etc/systemd/system/hypersion-bot.service
[Unit]
Description=Hypersion Lichess bot
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/hypersion/lichess_bot
ExecStart=/opt/hypersion/lichess_bot/venv/bin/python lichess_bot.py \
          --engine /opt/hypersion/Hypersion \
          --token-file /opt/hypersion/Hypersion.txt \
          --hash 512 --threads 1 \
          --log-file /var/log/hypersion-bot.log
Restart=always
RestartSec=10
User=hypersion

[Install]
WantedBy=multi-user.target
```

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `Token file not found` | the path doesn't exist or is unreadable |
| `Account is not a BOT` | run the curl `bot/account/upgrade` command |
| Engine never returns a move | check `Hypersion.exe` runs on its own (`bench`) |
| Lichess says "wrong move" | clock drift; `wtime/btime` extremely low |
| Disconnects mid-game | usually engine subprocess crashed — check logs |
| All challenges declined | check the filter list above; defaults are conservative |

## Limits

- **No variants.** Standard chess only.
- **Lazy SMP off.** `Threads=2` showed 30% disconnect rate in testing —
  keep `Threads=1` until SMP synchronization is fixed.
- **No opening book.** Hypersion supports `Perfect2023.bin` in the engine
  but the bot doesn't enable it; lichess opponents come from too varied
  an opening pool for a fixed book to help much.
- **No takebacks / unfinished-game recovery.** A handler crash → resign
  the current game; don't try to resume.
