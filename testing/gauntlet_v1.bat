@echo off
REM Mirror gauntlet match: Hypersion_v1 vs the same 4 opponents at the same
REM handicaps as gauntlet_lmrhist.bat. Pair them to measure absolute ELO change
REM on the LMR-history-correction port.

setlocal
set CC=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set HYPER=%~dp0Hypersion_v1.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin

set SF=C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe
set OBSIDIAN=C:\Engine\obsidian\Obsidian160-avx2.exe
set ALEXANDRIA=C:\Engine\alexandria\Alexandria-9.0-avx2.exe
set RUBI=C:\Engine\RubiChess-20240817\windows\RubiChess-20240817_x86-64-avx2.exe

set TC_HYPER=10+0.1
set TC_HANDICAP=2+0.02
set GAMES=30
set CONC=2

echo === V1 vs Stockfish (UCI_Elo=2400) ===
"%CC%" ^
  -engine name=Hyp_V1 cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=SF_2400 cmd="%SF%" tc=%TC_HYPER% option.UCI_LimitStrength=true option.UCI_Elo=2400 ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet_v1_vs_sf2400.pgn"

echo === V1 vs Obsidian 16 (TC handicap %TC_HANDICAP%) ===
"%CC%" ^
  -engine name=Hyp_V1 cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=Obsidian cmd="%OBSIDIAN%" tc=%TC_HANDICAP% ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet_v1_vs_obsidian.pgn"

echo === V1 vs Alexandria 9 (TC handicap %TC_HANDICAP%) ===
"%CC%" ^
  -engine name=Hyp_V1 cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=Alexandria cmd="%ALEXANDRIA%" tc=%TC_HANDICAP% ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet_v1_vs_alexandria.pgn"

echo === V1 vs RubiChess (LimitNps=100000) ===
"%CC%" ^
  -engine name=Hyp_V1 cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=RubiChess cmd="%RUBI%" tc=%TC_HYPER% option.LimitNps=100000 ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet_v1_vs_rubichess.pgn"

echo === V1 gauntlet complete ===
endlocal
