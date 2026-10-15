@echo off
rem EAWR online test: join. Asks for the 4-character code shown on the host.
title EAWR online test - join
cd /d "%~dp0"
eawr_online_test.exe join
echo.
echo Finished (exit code %ERRORLEVEL%). Send online-test-join.log if asked.
pause
