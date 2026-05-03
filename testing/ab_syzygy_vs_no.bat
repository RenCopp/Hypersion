@echo off
REM Isolated Syzygy A/B: same binary, one side has SyzygyPath set, other doesn't.
REM Measures pure tablebase ELO contribution since long-endgame losses are 45%
REM of all gauntlet losses (45% are 120+ ply, classic endgame weakness signature).

setlocal
set CC=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set HYPER=%~dp0Hypersion_lmrhist.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set SZPATH=C:\Engine\3-4-5 syzygy
set OUTPGN=%~dp0ab_syzygy_vs_no.pgn

set TC=10+0.1
set GAMES=200
set CONC=2

"%CC%" ^
  -engine name=Hyp_TB cmd="%HYPER%" option.OwnBook=false option.SyzygyPath="%SZPATH%" ^
  -engine name=Hyp_NoTB cmd="%HYPER%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 20 ^
  -pgnout "%OUTPGN%"

endlocal
