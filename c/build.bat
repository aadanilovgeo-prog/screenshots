@echo off
if not exist dist mkdir dist
python scripts\bump_version.py
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
for /f "usebackq delims=" %%v in ("..\VERSION") do set SC_VER=%%v
gcc -O2 -std=c11 -Wall -Wextra -Iinclude -o dist\scroll_capture_v%SC_VER%.exe ^
  src\main.c src\config.c src\image.c src\stitch.c src\capture.c src\png_io.c src\platform_win32.c ^
  -lgdi32 -luser32
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
echo Built dist\scroll_capture_v%SC_VER%.exe
