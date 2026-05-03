@echo off
REM Isolate-test: Lynx threats-aware butterfly history (fromAtt/toAtt bits)
REM ON TOP of current dev-main (= score-stab + bm-stab + bonus/malus split).

setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_threats.exe
set OLD=%~dp0Hypersion_devmain.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_threats_vs_devmain.pgn

set TC=10+0.1
set GAMES=40
set CONCURRENCY=2

"%CUTECHESS%" ^
  -engine name=Hyp_T  cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_DM cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONCURRENCY% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
