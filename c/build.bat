@echo off
setlocal
if not exist dist mkdir dist
for /f "usebackq delims=" %%v in ("..\VERSION") do set SC_VER=%%v

python scripts\generate_version_rc.py
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

set SRC=src\main.c src\config.c src\image.c src\stitch.c src\capture.c src\png_io.c src\platform_win32.c
set OUT=dist\scroll_capture_v%SC_VER%.exe
set RC=scroll_capture.rc
set RES=scroll_capture.res

where cl >nul 2>nul
if %ERRORLEVEL% EQU 0 (
  echo Building with MSVC...
  rc /nologo /fo %RES% %RC%
  if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
  cl /nologo /O2 /W4 /Iinclude %SRC% /Fe:%OUT% /link user32.lib gdi32.lib %RES%
  if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
  del /q *.obj 2>nul
  goto done
)

echo Building with GCC...
where windres >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
  echo windres not found. Install MinGW-w64 or use Visual Studio Build Tools.
  exit /b 1
)
windres -i %RC% -O coff -o %RES%
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
gcc -O2 -std=c11 -Wall -Wextra -Iinclude -o %OUT% %SRC% %RES% -lgdi32 -luser32 -s
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

:done
del /q %RES% 2>nul
echo Built %OUT% with VERSIONINFO manifest
endlocal
