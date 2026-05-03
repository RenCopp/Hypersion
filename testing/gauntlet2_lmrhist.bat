@echo off
REM Gauntlet v2: Hypersion_lmrhist vs top engines at HARSHER handicaps.
REM v1 gauntlet had Obsidian/Alexandria/RubiChess crushing both versions
REM identically (no signal). v2 calibration:
REM   - Stockfish: UCI_LimitStrength=true UCI_Elo=2400 (kept - worked)
REM   - Obsidian:   tc=0.5+0.005 (20x handicap, was 5x)
REM   - Alexandria: tc=0.5+0.005 (20x handicap, was 5x)
REM   - RubiChess:  LimitNps=10000 (was 100000)

setlocal
set CC=C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe
set HYPER=%~dp0Hypersion_lmrhist.exe
set OPENINGS=C:\Engine\cutechess-master\projects\lib\res\eco\eco.bin

set SF=C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe
set OBSIDIAN=C:\Engine\obsidian\Obsidian160-avx2.exe
set ALEXANDRIA=C:\Engine\alexandria\Alexandria-9.0-avx2.exe
set RUBI=C:\Engine\RubiChess-20240817\windows\RubiChess-20240817_x86-64-avx2.exe

set TC_HYPER=10+0.1
set TC_HEAVY=0.5+0.005
set GAMES=30
set CONC=2

echo === LMR vs Stockfish (UCI_Elo=2400) ===
"%CC%" ^
  -engine name=Hyp_LMR cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=SF_2400 cmd="%SF%" tc=%TC_HYPER% option.UCI_LimitStrength=true option.UCI_Elo=2400 ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet2_vs_sf2400.pgn"

echo === LMR vs Obsidian 16 (TC handicap %TC_HEAVY%) ===
"%CC%" ^
  -engine name=Hyp_LMR cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=Obsidian cmd="%OBSIDIAN%" tc=%TC_HEAVY% ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet2_vs_obsidian.pgn"

echo === LMR vs Alexandria 9 (TC handicap %TC_HEAVY%) ===
"%CC%" ^
  -engine name=Hyp_LMR cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=Alexandria cmd="%ALEXANDRIA%" tc=%TC_HEAVY% ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet2_vs_alexandria.pgn"

echo === LMR vs RubiChess (LimitNps=10000) ===
"%CC%" ^
  -engine name=Hyp_LMR cmd="%HYPER%" tc=%TC_HYPER% ^
  -engine name=RubiChess cmd="%RUBI%" tc=%TC_HYPER% option.LimitNps=10000 ^
  -each proto=uci option.Hash=64 option.Threads=1 ^
  -openings file="%OPENINGS%" format=epd order=random plies=8 ^
  -repeat -recover -concurrency %CONC% -games %GAMES% -ratinginterval 5 ^
  -pgnout "%~dp0gauntlet2_vs_rubichess.pgn"

echo === LMR gauntlet v2 complete ===
endlocal
