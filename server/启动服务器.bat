@echo off
chcp 65001 >nul
cd /d "%~dp0"

where py >nul 2>nul
if errorlevel 1 goto use_python

py server.py --open-browser
goto finished

:use_python
python server.py --open-browser

:finished
echo.
echo 服务器已停止。若上方显示错误，请截取完整内容排查。
pause
