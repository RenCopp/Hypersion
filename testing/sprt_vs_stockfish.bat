@echo off
REM SPRT (Sequential Probability Ratio Test) of Hypersion vs Stockfish.
REM Stops automatically once either bound is reached, or after MAX_GAMES.
REM
REM Tweak the variables below to taste.

REM Edit these paths to your local cutechess / Stockfish / openings install:
setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set HYPERSION=%~dp0..\Hypersion.exe
set STOCKFISH=C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0sprt_results.pgn

REM Time control: 10 seconds + 0.1 inc per move (fast SPRT, run longer for stronger signal)
set TC=10+0.1

REM SPRT bounds: H0 = elo0 (no improvement), H1 = elo1 (improvement of N elo)
REM Defaults match Stockfish OpenBench style: alpha = beta = 0.05
set ELO0=-5
set ELO1=10
set ALPHA=0.05
set BETA=0.05

REM Match cap.
set MAX_GAMES=2000
set CONCURRENCY=2

REM Hash & threads per engine.
set HASH=64
set THREADS=1

if not exist "%CUTECHESS%" (
    echo ERROR: cutechess-cli not found at %CUTECHESS%
    exit /b 1
)
if not exist "%HYPERSION%" (
    echo ERROR: Hypersion not found at %HYPERSION%
    exit /b 1
)
if not exist "%STOCKFISH%" (
    echo ERROR: Stockfish not found at %STOCKFISH% — adjust the path
    exit /b 1
)

"%CUTECHESS%" ^
  -engine name=Hypersion cmd="%HYPERSION%" ^
  -engine name=Stockfish cmd="%STOCKFISH%" ^
  -each proto=uci tc=%TC% option.Hash=%HASH% option.Threads=%THREADS% ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat ^
  -recover ^
  -concurrency %CONCURRENCY% ^
  -games %MAX_GAMES% ^
  -ratinginterval 10 ^
  -sprt elo0=%ELO0% elo1=%ELO1% alpha=%ALPHA% beta=%BETA% ^
  -pgnout "%OUTPGN%"

endlocal
