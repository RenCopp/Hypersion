@echo off
REM 200-game cumulative dev-main vs v1.0 — tight ELO bounds.
REM On a 14700F at 10+0.1 with concurrency 2, this runs in ~35 min.
REM 95%% CI on 200 games ≈ ±40 ELO (vs ±90 on 40g).

setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_devmain.exe
set OLD=%~dp0Hypersion_v1.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_200g_vs_v1.pgn

set TC=10+0.1
set GAMES=200
set CONCURRENCY=2

"%CUTECHESS%" ^
  -engine name=Hyp_DEV cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_V1  cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONCURRENCY% -games %GAMES% -ratinginterval 20 ^
  -pgnout "%OUTPGN%"

endlocal
