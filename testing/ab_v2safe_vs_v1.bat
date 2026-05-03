@echo off
REM v2 conservative (insufficient-material draw + go searchmoves) vs v1.0
REM Reverted the risky changes (worsening, score-drop, history-decay) that
REM lost -35 ELO when bundled.

setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_v2_safe.exe
set OLD=%~dp0Hypersion_v1.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_v2safe_vs_v1.pgn

set TC=10+0.1
set GAMES=40
set CONCURRENCY=2

"%CUTECHESS%" ^
  -engine name=Hyp_V2S cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_V1  cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONCURRENCY% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
