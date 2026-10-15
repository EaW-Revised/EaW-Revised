@echo off
rem EAWR online test: host. Starts a test match and shows a 4-character code for the other player.
title EAWR online test - host
cd /d "%~dp0"
eawr_online_test.exe host
echo.
echo Finished (exit code %ERRORLEVEL%). Send online-test-host.log if asked.
pause
