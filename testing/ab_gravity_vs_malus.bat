@echo off
REM Isolate-test: Lynx 3/4 history gravity ON TOP of bonus/malus split.

setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_gravity.exe
set OLD=%~dp0Hypersion_malus.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_gravity_vs_malus.pgn

set TC=10+0.1
set GAMES=40
set CONCURRENCY=2

"%CUTECHESS%" ^
  -engine name=Hyp_G cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_M cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONCURRENCY% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
