@echo off
rem Double-click to start the worker watchdog (hidden, detached). Does nothing if it already runs.
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Watchdog.ps1" %*
pause
