# Running Hypersion as a Lichess bot

`lichess_bot/` ships a Python wrapper that connects Hypersion to the
Lichess BOT API. It auto-matches its strength to the opponent, chats
hello / gg, tracks W/D/L, and auto-restarts the engine if it crashes.

## Prerequisites

1. **Python 3.10+** with `pip` and `venv`. On Windows, the `py`
   launcher (default with python.org installers) is used by `run.bat`.
2. **The Hypersion executable** — build it via `make build`.
3. **The two NNUE files** — see [NNUE.md](NNUE.md).
4. **A Lichess account dedicated to the bot.** It must be a fresh
   account that has played **no rated games**, otherwise Lichess won't
   let it become a bot. Casual games are fine.

## One-time setup

### 1. Get an API token

At <https://lichess.org/account/oauth/token/create>, create a token
with the **`bot:play`** scope. Lichess will show the token once —
copy it immediately.

### 2. Save the token

Save the token as a single line in `Hypersion.txt` at the project root
(same directory as `Hypersion.exe`). Lichess tokens have the format
`lip_` followed by 20 alphanumeric characters.

The bot's default `--token-file` path is `../Hypersion.txt` relative to
`lichess_bot/run.bat`, which works out to the project root.

### 3. Upgrade the account to a BOT account

This is **one-way and permanent**. Run:

```sh
curl -d "" https://lichess.org/api/bot/account/upgrade \
     -H "Authorization: Bearer YOUR_TOKEN_HERE"
```

Or in PowerShell:

```powershell
$token = (Get-Content Hypersion.txt -TotalCount 1).Trim()
Invoke-RestMethod -Uri "https://lichess.org/api/bot/account/upgrade" `
                  -Headers @{ "Authorization" = "Bearer $token" } `
                  -Method POST -Body ""
```

The expected response is `{"ok": true}`. After this, the account is a
BOT and can no longer play as a regular human account.

## Running

On Windows:

```bat
cd lichess_bot
run.bat
```

On Linux / macOS:

```sh
cd lichess_bot
python -m venv venv
. venv/bin/activate
pip install -r requirements.txt
python lichess_bot.py --engine ../Hypersion --token-file ../Hypersion.txt \
                     --hash 256 --threads 1 --max-games 3 --accept-rated
```

The bot will sit in the foreground waiting for events. Send a challenge
to the bot from a different account and it will accept (subject to the
filters below).

## Command-line flags

| Flag | Default | Notes |
|---|---|---|
| `--engine PATH` | `../Hypersion.exe` (Windows) or `../Hypersion` | path to the executable |
| `--token-file PATH` | `../Hypersion.txt` | API token file (one line) |
| `--hash MB` | 128 | engine hash table |
| `--threads N` | 1 | UCI Threads — **leave at 1**, lazy-SMP is buggy |
| `--accept-rated` | off | accept rated challenges (affects the bot's lichess rating) |
| `--max-games N` | 2 | concurrent-game cap |
| `--max-tc-min N` | 0 | reject TC below N minutes (0 = accept all) |
| `--log-file PATH` | none | mirror logs to a file in addition to stdout |
| `--no-chat` | off | disable game-start hello + game-end thanks |

## Adaptive strength

Per game, the bot reads the opponent's rating from the `gameFull`
event and decides what strength to play at. The cap is implemented
engine-side via UCI `UCI_LimitStrength` + `UCI_Elo`.

| Opponent | Target Elo |
|---|---|
| Bot opponent (`title=BOT`) | full strength |
| Provisional rating | max(rating, 1700) — ratings on new accounts are noisy |
| Rated ≥ 2500 | full strength |
| Casual / unrated | 1500 baseline |
| Otherwise | match opponent's rating |
| Bullet TC | minimum floor of 1100 |

NNUE remains loaded throughout. Strength is reduced via depth cap +
move-selection noise, not by disabling the network.

## Default challenge filtering

The bot **declines** by default:

- Variant games (anything not standard chess)
- Unlimited time control
- Rated games (override with `--accept-rated`)
- Time control below `--max-tc-min`
- Challenges that would exceed `--max-games`

Edit `lichess_bot/lichess_bot.py` to customise.

## Profile and account settings

`lichess_bot/PROFILE.md` has copy-pasteable bio templates and recommended
account preferences for a bot account.

## Running as a service

On Linux you can use systemd. Sample unit file is in
`lichess_bot/README.md`. On Windows, `nssm` (Non-Sucking Service Manager)
or Task Scheduler both work — point them at `run.bat`.

## Common problems

| Symptom | Likely cause |
|---|---|
| `Token file not found` | the path doesn't exist or is unreadable |
| `Account is not a BOT` | run the `bot/account/upgrade` curl |
| Engine never returns a move | run `Hypersion bench` standalone first to verify it works |
| All challenges declined | open `lichess_bot.py` and check the filter list |
| Disconnects mid-game | usually the engine subprocess crashed — check logs; the bot auto-restarts the engine on the next game |
