@echo off
if not exist dist mkdir dist
gcc -O2 -std=c11 -Wall -Wextra -Iinclude -o dist\updated_screenshoter.exe ^
  src\main.c src\config.c src\image.c src\metrics.c src\stitch.c src\session.c ^
  src\platform_win32.c src\png_io.c ^
  -lgdi32 -luser32 -lole32 -lm
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
echo Built dist\updated_screenshoter.exe
