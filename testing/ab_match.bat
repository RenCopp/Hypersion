@echo off
REM A/B match: candidate Hypersion.exe vs baseline Hypersion_v0.exe.
REM 100 games at 10+0.1, ~15 min wall time on a modern CPU.
REM
REM Pass an optional label as %1 to tag the result PGN.
REM   testing\ab_match.bat "phase1-backward-pawn"

REM Edit these paths to your local cutechess / opponent / openings install:
setlocal
set CUTECHESS=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set NEW=%~dp0..\Hypersion.exe
set OLD=%~dp0Hypersion_v0.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin
set LABEL=%~1
if "%LABEL%"=="" set LABEL=match
set OUTPGN=%~dp0ab_%LABEL%.pgn

set TC=60+0.6
set GAMES=20
set CONCURRENCY=2
set HASH=64
set THREADS=1

if not exist "%CUTECHESS%" ( echo ERROR: cutechess not at %CUTECHESS% & exit /b 1 )
if not exist "%NEW%" ( echo ERROR: candidate not at %NEW% & exit /b 1 )
if not exist "%OLD%" ( echo ERROR: baseline not at %OLD% & exit /b 1 )

"%CUTECHESS%" ^
  -engine name=Hyp_NEW cmd="%NEW%" option.OwnBook=false ^
  -engine name=Hyp_OLD cmd="%OLD%" option.OwnBook=false ^
  -each proto=uci tc=%TC% option.Hash=%HASH% option.Threads=%THREADS% ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat ^
  -recover ^
  -concurrency %CONCURRENCY% ^
  -games %GAMES% ^
  -ratinginterval 20 ^
  -pgnout "%OUTPGN%"

endlocal
