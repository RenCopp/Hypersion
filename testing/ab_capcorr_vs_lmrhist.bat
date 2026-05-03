@echo off
REM 200-game isolated A/B: capCorr (LMR + capture-history correction) vs lmrhist
REM (LMR quiet-only). Same TC / openings / hash as previous matches.
REM Measures the marginal effect of adding capture-side LMR correction on
REM top of the proven quiet-side correction.

setlocal
set CC=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_capcorr.exe
set OLD=%~dp0Hypersion_lmrhist.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_capcorr_vs_lmrhist.pgn

set TC=10+0.1
set GAMES=200
set CONC=2

"%CC%" ^
  -engine name=Hyp_CAP cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_LMR cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 20 ^
  -pgnout "%OUTPGN%"

endlocal
