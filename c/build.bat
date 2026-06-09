@echo off
gcc -O2 -std=c11 -Wall -Wextra -Iinclude -o scroll_capture.exe ^
  src/main.c src/config.c src/image.c src/stitch.c src/capture.c src/png_io.c src/platform_win32.c ^
  -lgdi32 -luser32
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
echo Built scroll_capture.exe
