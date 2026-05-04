@echo off
REM 50-game match: Hypersion vs FULL-STRENGTH Stockfish.
REM SF will dominate (~3500 vs Hyp's ~2660 = +800 ELO gap, expect Hyp lose
REM most games), but every loss is a learning sample. We then run SF
REM analysis on every game to find Hyp's mistakes.

setlocal
set CC=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set HYPER=%~dp0Hypersion_repfix.exe
set SF=C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0match_50g_vs_sf.pgn

set TC=10+0.1
set GAMES=50
set CONC=2

"%CC%" ^
  -engine name=Hypersion cmd="%HYPER%" ^
  -engine name=Stockfish cmd="%SF%" ^
  -each proto=uci tc=%TC% option.Hash=128 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
