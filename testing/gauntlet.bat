@echo off
REM Gauntlet: Hypersion vs a small set of fixed opponents — useful for ELO calibration
REM before running SPRT against Stockfish.
REM
REM Drop opponent .exe files into C:\Engine\opponents\ then run.

REM Edit these paths to your local cutechess / opponent / openings install:
setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set HYPERSION=%~dp0..\Hypersion.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0gauntlet_results.pgn

set TC=10+0.1
set GAMES_PER_OPPONENT=50
set CONCURRENCY=2

if not exist "%CUTECHESS%" (
    echo ERROR: cutechess-cli not found
    exit /b 1
)

REM Add opponents on -engine lines below — copy and edit one for each opponent.
"%CUTECHESS%" ^
  -engine name=Hypersion cmd="%HYPERSION%" ^
  -engine name=Stockfish cmd="C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe" ^
  -engine name=HybridChess cmd="C:\Engine\Hybird engine V17\HybridChess_v17.exe" ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat ^
  -recover ^
  -concurrency %CONCURRENCY% ^
  -tournament gauntlet ^
  -games %GAMES_PER_OPPONENT% ^
  -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
