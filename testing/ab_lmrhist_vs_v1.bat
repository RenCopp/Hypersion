@echo off
REM 200-game A/B: LMR history correction (quiet moves) vs v1.0 baseline.
REM Same TC / openings / hash as the dev-main vs v1 run that gave -12.2 ELO.
REM 95%% CI on 200 games ~ +/- 40 ELO.

setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_lmrhist.exe
set OLD=%~dp0Hypersion_v1.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_lmrhist_vs_v1.pgn

set TC=10+0.1
set GAMES=200
set CONCURRENCY=2

"%CUTECHESS%" ^
  -engine name=Hyp_LMR cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_V1  cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONCURRENCY% -games %GAMES% -ratinginterval 20 ^
  -pgnout "%OUTPGN%"

endlocal
