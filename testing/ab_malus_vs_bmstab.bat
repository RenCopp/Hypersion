@echo off
REM Isolate-test: Lynx split bonus/malus formulas ON TOP of bm-stab+score-stab.
REM   malus binary  = Lynx port + split bonus/malus formulas
REM   bmstab binary = Lynx port without split

setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_malus.exe
set OLD=%~dp0Hypersion_bmstab.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_malus_vs_bmstab.pgn

set TC=10+0.1
set GAMES=40
set CONCURRENCY=2

"%CUTECHESS%" ^
  -engine name=Hyp_M  cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_BM cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONCURRENCY% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
