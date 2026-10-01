@echo off
setlocal
py -3 -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul
if not errorlevel 1 (
  py -3 "%~dp0demo.py" %*
) else (
  python -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul
  if errorlevel 1 (
    echo Install Python 3.8 or newer, then run play-demo.cmd again.
    call :pause_if_interactive
    exit /b 2
  )
  python "%~dp0demo.py" %*
)
if errorlevel 1 (
  call :pause_if_interactive
  exit /b 2
)
exit /b 0

:pause_if_interactive
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "if ([Console]::IsInputRedirected) { exit 1 }" >nul 2>nul
if not errorlevel 1 pause
exit /b 0
