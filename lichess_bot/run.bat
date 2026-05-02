@echo off
REM Hypersion Lichess bot launcher.
REM Edit BOT_FLAGS below to taste.

setlocal
set BOT_DIR=%~dp0
set ENGINE=%BOT_DIR%..\Hypersion.exe
set TOKEN=%BOT_DIR%..\Hypersion.txt
set BOT_FLAGS=--hash 256 --threads 1 --max-games 3 --accept-rated

REM Pick a Python: prefer the `py` launcher (default on Windows installs);
REM fall back to `python` if it's actually on PATH (not the Microsoft Store stub).
set PY=
where py >nul 2>nul && set PY=py
if "%PY%"=="" ( where python >nul 2>nul && set PY=python )
if "%PY%"=="" (
    echo ERROR: No Python found. Install Python 3.10+ from python.org and re-run.
    exit /b 1
)

REM First-time setup: create venv + install deps.
if not exist "%BOT_DIR%venv" (
    echo Creating Python virtualenv...
    %PY% -m venv "%BOT_DIR%venv"
    "%BOT_DIR%venv\Scripts\python" -m pip install -U pip
    "%BOT_DIR%venv\Scripts\pip" install -r "%BOT_DIR%requirements.txt"
)

"%BOT_DIR%venv\Scripts\python" "%BOT_DIR%lichess_bot.py" --engine "%ENGINE%" --token-file "%TOKEN%" %BOT_FLAGS% %*
endlocal
