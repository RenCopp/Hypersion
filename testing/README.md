# Testing scripts

Cute Chess CLI launchers for ELO calibration. **Each script has paths
near the top that need editing for your environment** — at minimum
`CUTECHESS` (path to `cutechess-cli.exe`) and `OPENINGS` (path to an
opening EPD/PGN). The Hypersion path defaults to `..\Hypersion.exe`
relative to this folder.

| Script | Purpose |
|---|---|
| `ab_match.bat [LABEL]` | Hypersion vs `Hypersion_v0.exe` (drop a baseline binary in this folder), 60+0.6 × 20 games. Useful for measuring incremental change. |
| `gauntlet.bat` | Round-robin gauntlet against multiple opponents (Stockfish, others). |
| `sprt_vs_stockfish.bat` | Sequential Probability Ratio Test vs Stockfish, autostops on bound. |

## Requirements

- [Cute Chess](https://cutechess.com/) — `cutechess-cli` binary
- Opening book in EPD or PGN format (e.g. ECO from the Cute Chess source tree)
- For `gauntlet.bat` / `sprt_vs_stockfish.bat`: the opponent engines

## Output

Each script writes a `.pgn` next to itself with the games. Use
[ordo](https://github.com/michiguel/Ordo) or the `bayeselo` tool to turn
the PGN into ELO numbers, or just import it into Cute Chess GUI to
browse the games.

## Notes

- The scripts default to `Threads=1` — Hypersion's lazy-SMP is currently
  unstable and produces disconnects with `Threads>=2`.
- `Hash=64` MB is enough for 10+0.1; raise to 256 MB for slow TC.
- `concurrency=2` works on 4-core+ CPUs; raise to match your physical-core count.
