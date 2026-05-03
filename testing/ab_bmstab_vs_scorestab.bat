@echo off
REM Isolate-test: Lynx bm-stab buckets ON TOP of score-stab.
REM   bm-stab binary  = score-stab + 5-bucket bestmove-stability
REM   score-stab binary = Lynx score-stab only
REM Anything beyond noise is purely the bm-stab change.

setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_bmstab.exe
set OLD=%~dp0Hypersion_scorestab.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_bmstab_vs_scorestab.pgn

set TC=10+0.1
set GAMES=40
set CONCURRENCY=2

"%CUTECHESS%" ^
  -engine name=Hyp_BM cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_SS cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONCURRENCY% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
