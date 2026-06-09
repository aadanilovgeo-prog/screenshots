# scroll_capture (C)

Нативная Windows-версия утилиты захвата длинных статей. Функциональность совпадает с `scroll_capture.py`:

- выбор области (интерактивно или `--region`)
- один клик при старте для фокуса
- прокрутка колёсиком (`WM_MOUSEWHEEL`)
- серия скриншотов и склейка с выравниванием по тексту/таблицам
- сохранение PNG

## Готовый EXE

Скачайте из репозитория:

```
c/dist/scroll_capture_v1.0.0.exe
```

Имя exe включает версию: `scroll_capture_v{VERSION}.exe`

## Сборка (Windows)

```bat
cd c
build.bat
```

Создаст `dist\scroll_capture_v1.0.1.exe` (версия автоматически увеличивается).

## Запуск

```bat
scroll_capture_v1.0.0.exe
scroll_capture_v1.0.0.exe --version
scroll_capture_v1.0.0.exe --region 120,80,900,700 -o article.png
```

## Version

Версия хранится в `/VERSION`. Каждая сборка (`build.bat`, `make windows`, CI) увеличивает patch-номер и обновляет имя exe.
