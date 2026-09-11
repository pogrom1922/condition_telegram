@echo off
setlocal
cd /d "%~dp0"

if not exist config.py (
  echo ERROR: config.py not found.
  echo Copy config.example.py to config.py and fill BOT_TOKEN and ALLOWED_USER_IDS.
  pause
  exit /b 1
)

where py >nul 2>&1
if %errorlevel%==0 (
  set PY=py -3
) else (
  set PY=python
)

if not exist .venv\Scripts\python.exe (
  %PY% -m venv .venv
  call .venv\Scripts\activate.bat
  python -m pip install --upgrade pip
  pip install -r requirements.txt
) else (
  call .venv\Scripts\activate.bat
)

python bot.py
pause
