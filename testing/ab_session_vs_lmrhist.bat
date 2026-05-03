@echo off
REM Cumulative A/B: session changes (Threads=2 default + SMP diversity + ttPv +
REM bug fixes + stdout unbuffering) vs lmrhist baseline (just LMR quiet correction).
REM Both use Threads=1 to isolate per-position search behavior from SMP gains.

setlocal
set CC=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0Hypersion_ttpv.exe
set OLD=%~dp0Hypersion_lmrhist.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set OUTPGN=%~dp0ab_session_vs_lmrhist.pgn

set TC=10+0.1
set GAMES=30
set CONC=2

"%CC%" ^
  -engine name=Hyp_NEW cmd="%NEW%" option.OwnBook=false option.Threads=1 ^
  -engine name=Hyp_LMR cmd="%OLD%" option.OwnBook=false option.Threads=1 ^
  -each proto=uci tc=%TC% option.Hash=64 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%OUTPGN%"

endlocal
