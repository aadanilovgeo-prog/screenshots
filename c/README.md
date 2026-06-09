# scroll_capture (C)

Нативная Windows-версия утилиты захвата длинных статей. Функциональность совпадает с `scroll_capture.py`:

- выбор области (интерактивно или `--region`)
- один клик при старте для фокуса
- прокрутка колёсиком (`WM_MOUSEWHEEL`)
- серия скриншотов и склейка с выравниванием по тексту/таблицам
- сохранение PNG

## Сборка (Windows)

Нужен [MinGW-w64](https://www.mingw-w64.org/) или MSYS2 с `gcc` в PATH.

```bat
cd c
build.bat
```

Или вручную:

```bat
gcc -O2 -std=c11 -Wall -Wextra -Iinclude -o scroll_capture.exe ^
  src/main.c src/config.c src/image.c src/stitch.c src/capture.c src/png_io.c src/platform_win32.c ^
  -lgdi32 -luser32
```

## Запуск

```bat
scroll_capture.exe
```

С параметрами (те же, что у Python-версии):

```bat
scroll_capture.exe --region 120,80,900,700 -o article.png
scroll_capture.exe --wheel-notches 10 --save-frames frames
```

## Преимущества C-версии

- один `.exe` без Python и pip
- быстрее захват экрана и склейка
- меньше потребление памяти на длинных статьях

## Зависимости

Только Win32 API и встроенный `stb_image_write` (лежит в `third_party/`).
