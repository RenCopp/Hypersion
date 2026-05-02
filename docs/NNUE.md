# NNUE network files

Hypersion's evaluation needs two network files in the engine's working
directory:

| File | Size | Role |
|---|---|---|
| `nn-c288c895ea92.nnue` | ~104 MB | big network — used for normal positions |
| `nn-37f18f62d772.nnue` | ~3 MB | small network — used in lopsided endgames |

These are the same network files that **Stockfish 18** uses. They are
**GPL-3 licensed** and not bundled in this repository to keep clones
small.

## Where to download

The canonical source is the Stockfish download server:

| File | Direct link |
|---|---|
| Big | <https://tests.stockfishchess.org/api/nn/nn-c288c895ea92.nnue> |
| Small | <https://tests.stockfishchess.org/api/nn/nn-37f18f62d772.nnue> |

Mirrors of the same files often appear at:

- <https://github.com/official-stockfish/networks/releases>
- The Fishtest network archive (linked from the SF README)

Download both and place them in the same directory as the `Hypersion`
executable (or anywhere the engine's working directory will be at
runtime).

## On Linux / macOS

```sh
curl -L -O https://tests.stockfishchess.org/api/nn/nn-c288c895ea92.nnue
curl -L -O https://tests.stockfishchess.org/api/nn/nn-37f18f62d772.nnue
```

## On Windows (PowerShell)

```powershell
Invoke-WebRequest -Uri "https://tests.stockfishchess.org/api/nn/nn-c288c895ea92.nnue" -OutFile "nn-c288c895ea92.nnue"
Invoke-WebRequest -Uri "https://tests.stockfishchess.org/api/nn/nn-37f18f62d772.nnue" -OutFile "nn-37f18f62d772.nnue"
```

## Verifying

When Hypersion starts (e.g. via `./Hypersion uci`), it prints the network
header for each file:

```
info string nnue arch: Network trained with the https://github.com/official-stockfish/nnue-pytorch trainer.
info string nnue: FT hash 0x8f2344b8
info string nnue: threat weights loaded (77MB)
info string nnue: FT loaded
info string nnue evaluation using nn-c288c895ea92.nnue (103MiB, (102384, 1024, 15, 32, 1))
info string nnue arch: Network trained with the https://github.com/official-stockfish/nnue-pytorch trainer.
info string nnue: FT hash 0x7f234db8
info string nnue: FT loaded
info string nnue evaluation using nn-37f18f62d772.nnue (3MiB, (22528, 128, 15, 32, 1))
```

If you see only the classical evaluator (no `info string nnue` lines),
the files weren't found. Check the working directory or pass explicit
paths via UCI:

```
setoption name EvalFile      value /path/to/nn-c288c895ea92.nnue
setoption name EvalFileSmall value /path/to/nn-37f18f62d772.nnue
```

## Why these specific files?

Hypersion's NNUE inference code expects the **SF18 SFNNv10** binary
format (HalfKAv2_hm features + FullThreats, 1024-d FT for the big net,
128-d FT for the small net). Other Stockfish networks may use different
architectures and won't load.

If a future Stockfish release ships a successor network with the same
architecture, the file should drop in — but this isn't guaranteed,
because SF periodically rotates architectures.

## License

The network files are released under GPL-3 by the Stockfish project. By
using them with Hypersion (which is also GPL-3) you are within the
license terms.
