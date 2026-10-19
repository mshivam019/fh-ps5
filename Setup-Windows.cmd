@echo off
set PYTHONUTF8=1
cd /d "%~dp0"
where py >nul 2>nul
if not errorlevel 1 (
  py -3 "%~dp0ps5\tools\setup.py" %*
) else (
  python "%~dp0ps5\tools\setup.py" %*
)
if errorlevel 1 echo Setup stopped. See build\setup\setup.log. Python 3.11+ and Git for Windows are required.
pause
