#!/usr/bin/env python3
"""
Автоматический захват длинной страницы: прокрутка мышью, скриншоты, склейка в один файл.

Предназначено для сценариев вроде VRM/удалённого рабочего стола, когда нельзя
скопировать текст или выгрузить файл, но можно просматривать страницу на экране.
"""

from __future__ import annotations

import argparse
import ctypes
import platform
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import mss
import numpy as np
import pyautogui
from PIL import Image


pyautogui.FAILSAFE = True
pyautogui.PAUSE = 0.02

WHEEL_DELTA = 120
MOUSEEVENTF_WHEEL = 0x0800


@dataclass
class Region:
    left: int
    top: int
    width: int
    height: int

    @property
    def right(self) -> int:
        return self.left + self.width

    @property
    def bottom(self) -> int:
        return self.top + self.height

    @property
    def center(self) -> tuple[int, int]:
        return self.left + self.width // 2, self.top + self.height // 2

    def as_mss_monitor(self) -> dict[str, int]:
        return {
            "left": self.left,
            "top": self.top,
            "width": self.width,
            "height": self.height,
        }


@dataclass
class ScrollSettings:
    method: str
    wheel_notches: int
    micro_steps: int
    micro_delay: float
    focus_click: bool
    focus_before_each_step: bool


def parse_region(value: str) -> Region:
    parts = [int(part.strip()) for part in value.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("Формат области: left,top,width,height")
    left, top, width, height = parts
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("Ширина и высота должны быть больше нуля")
    return Region(left, top, width, height)


def default_scroll_method() -> str:
    if platform.system() == "Windows":
        return "win32"
    return "pynput"


def pick_region_interactive() -> Region:
    print("Интерактивный выбор области захвата.")
    print("Наведите курсор на ЛЕВЫЙ ВЕРХНИЙ угол статьи и нажмите Enter...")
    input()
    x1, y1 = pyautogui.position()
    print(f"  Верхний левый угол: ({x1}, {y1})")

    print("Наведите курсор на ПРАВЫЙ НИЖНИЙ угол статьи и нажмите Enter...")
    input()
    x2, y2 = pyautogui.position()
    print(f"  Нижний правый угол: ({x2}, {y2})")

    left, top = min(x1, x2), min(y1, y2)
    width, height = abs(x2 - x1), abs(y2 - y1)
    if width < 50 or height < 50:
        raise SystemExit("Слишком маленькая область. Выберите прямоугольник побольше.")
    return Region(left, top, width, height)


def countdown(seconds: int, message: str) -> None:
    print(message)
    for remaining in range(seconds, 0, -1):
        print(f"  {remaining}...", flush=True)
        time.sleep(1)
    print("  Старт!")


def grab_region(sct: mss.mss, region: Region) -> Image.Image:
    shot = sct.grab(region.as_mss_monitor())
    return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def image_diff_ratio(a: Image.Image, b: Image.Image) -> float:
    arr_a = np.asarray(a, dtype=np.int16)
    arr_b = np.asarray(b, dtype=np.int16)
    if arr_a.shape != arr_b.shape:
        return 1.0
    return float(np.mean(np.abs(arr_a - arr_b)) / 255.0)


def find_vertical_overlap(img_above: Image.Image, img_below: Image.Image, max_search: int) -> int:
    width = img_above.width
    height = min(img_above.height, img_below.height)
    max_search = min(max_search, height - 20)
    if max_search < 20:
        return 0

    above = np.asarray(img_above, dtype=np.int16)
    below = np.asarray(img_below, dtype=np.int16)

    best_overlap = 0
    best_score = float("inf")

    for overlap in range(20, max_search + 1):
        region_above = above[height - overlap : height, :width]
        region_below = below[:overlap, :width]
        score = float(np.mean(np.abs(region_above - region_below)))
        if score < best_score:
            best_score = score
            best_overlap = overlap

    return best_overlap


def stitch_frames(frames: list[Image.Image], expected_overlap: int) -> Image.Image:
    if not frames:
        raise ValueError("Нет кадров для склейки")
    if len(frames) == 1:
        return frames[0].copy()

    search_range = max(expected_overlap + 80, frames[0].height // 2)
    pieces: list[Image.Image] = [frames[0]]

    for frame in frames[1:]:
        overlap = find_vertical_overlap(pieces[-1], frame, search_range)
        if overlap <= 0:
            overlap = min(expected_overlap, frame.height - 1)
        pieces.append(frame.crop((0, overlap, frame.width, frame.height)))

    total_height = sum(img.height for img in pieces)
    result = Image.new("RGB", (pieces[0].width, total_height))
    y = 0
    for piece in pieces:
        result.paste(piece, (0, y))
        y += piece.height
    return result


def focus_region(region: Region) -> None:
    x, y = region.center
    pyautogui.moveTo(x, y, duration=0.08)
    time.sleep(0.05)
    pyautogui.click(x, y)
    time.sleep(0.08)


def _win32_wheel_at(x: int, y: int, notches_down: int) -> None:
    user32 = ctypes.windll.user32
    user32.SetCursorPos(x, y)
    # Отрицательное значение = прокрутка вниз (WM_MOUSEWHEEL).
    delta = int(-WHEEL_DELTA * notches_down)
    user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, delta & 0xFFFFFFFF, 0)


def _pynput_wheel_at(x: int, y: int, notches_down: int) -> None:
    from pynput.mouse import Controller

    mouse = Controller()
    mouse.position = (x, y)
    time.sleep(0.02)
    mouse.scroll(0, -notches_down)


def _pyautogui_wheel_at(x: int, y: int, notches_down: int) -> None:
    pyautogui.moveTo(x, y, duration=0.02)
    time.sleep(0.02)
    pyautogui.scroll(-notches_down)


def emit_wheel_at(x: int, y: int, notches_down: int, method: str) -> None:
    if notches_down <= 0:
        return
    if method == "win32":
        _win32_wheel_at(x, y, notches_down)
    elif method == "pynput":
        _pynput_wheel_at(x, y, notches_down)
    elif method == "pyautogui":
        _pyautogui_wheel_at(x, y, notches_down)
    else:
        raise ValueError(f"Неизвестный метод прокрутки: {method}")


def scroll_wheel_at(region: Region, settings: ScrollSettings) -> None:
    x, y = region.center

    if settings.focus_click or settings.focus_before_each_step:
        focus_region(region)

    steps = max(1, settings.micro_steps)
    base = settings.wheel_notches // steps
    remainder = settings.wheel_notches % steps

    for step_index in range(steps):
        notches = base + (1 if step_index < remainder else 0)
        if notches <= 0:
            continue
        emit_wheel_at(x, y, notches, settings.method)
        time.sleep(settings.micro_delay)


def capture_long_page(
    region: Region,
    scroll: ScrollSettings,
    *,
    scroll_delay: float,
    settle_delay: float,
    max_frames: int,
    same_frame_threshold: float,
    save_frames_dir: Path | None,
) -> tuple[list[Image.Image], bool]:
    frames: list[Image.Image] = []
    reached_end = False

    if scroll.focus_click:
        print("Фокус на области статьи (клик мышью)...")
        focus_region(region)
        time.sleep(0.2)

    with mss.mss() as sct:
        print("Захват первого кадра...")
        previous = grab_region(sct, region)
        frames.append(previous.copy())
        if save_frames_dir:
            previous.save(save_frames_dir / "frame_0000.png")

        for index in range(1, max_frames + 1):
            scroll_wheel_at(region, scroll)
            time.sleep(scroll_delay)

            current = grab_region(sct, region)
            time.sleep(settle_delay)

            diff = image_diff_ratio(previous, current)
            print(f"  Кадр {index:04d}: отличие от предыдущего {diff * 100:.2f}%")

            if diff <= same_frame_threshold:
                print("Страница, похоже, достигла конца (кадры совпадают).")
                reached_end = True
                break

            frames.append(current.copy())
            if save_frames_dir:
                current.save(save_frames_dir / f"frame_{index:04d}.png")
            previous = current

    return frames, reached_end


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Автопрокрутка страницы в VRM/удалённом рабочем столе и склейка скриншотов.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--region",
        type=parse_region,
        help="Область захвата: left,top,width,height (в пикселях экрана)",
    )
    parser.add_argument(
        "--output",
        "-o",
        type=Path,
        help="Путь к итоговому PNG (по умолчанию scroll_capture_ДАТА.png)",
    )
    parser.add_argument(
        "--countdown",
        type=int,
        default=5,
        help="Секунд до старта — успейте переключиться в окно VRM",
    )
    parser.add_argument(
        "--scroll-method",
        choices=("auto", "win32", "pynput", "pyautogui"),
        default="auto",
        help="Способ имитации колёсика мыши (win32 — нативный Windows API)",
    )
    parser.add_argument(
        "--scroll-clicks",
        "--wheel-notches",
        dest="wheel_notches",
        type=int,
        default=12,
        help="Сколько «щелчков» колёсика за один шаг прокрутки",
    )
    parser.add_argument(
        "--micro-steps",
        type=int,
        default=8,
        help="Разбить один шаг на несколько коротких прокруток колёсиком",
    )
    parser.add_argument(
        "--micro-delay",
        type=float,
        default=0.04,
        help="Пауза между микро-прокрутками, сек",
    )
    parser.add_argument(
        "--no-focus-click",
        action="store_true",
        help="Не кликать в область статьи перед прокруткой (обычно клик нужен)",
    )
    parser.add_argument(
        "--focus-each-step",
        action="store_true",
        help="Кликать в область перед каждым шагом (для кастомных скроллеров)",
    )
    parser.add_argument(
        "--scroll-delay",
        type=float,
        default=0.45,
        help="Пауза после прокрутки перед скриншотом, сек",
    )
    parser.add_argument(
        "--settle-delay",
        type=float,
        default=0.15,
        help="Дополнительная пауза после скриншота, сек",
    )
    parser.add_argument(
        "--max-frames",
        type=int,
        default=300,
        help="Максимум кадров (защита от бесконечного цикла)",
    )
    parser.add_argument(
        "--same-frame-threshold",
        type=float,
        default=0.002,
        help="Порог совпадения кадров (0..1). Меньше = строже определение конца страницы",
    )
    parser.add_argument(
        "--expected-overlap",
        type=int,
        default=0,
        help="Ожидаемое перекрытие в пикселях для склейки (0 = автоопределение)",
    )
    parser.add_argument(
        "--save-frames",
        type=Path,
        help="Папка для сохранения отдельных кадров (для отладки)",
    )
    return parser


def resolve_scroll_method(requested: str) -> str:
    if requested != "auto":
        return requested
    return default_scroll_method()


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    region = args.region or pick_region_interactive()
    scroll_method = resolve_scroll_method(args.scroll_method)
    scroll = ScrollSettings(
        method=scroll_method,
        wheel_notches=args.wheel_notches,
        micro_steps=args.micro_steps,
        micro_delay=args.micro_delay,
        focus_click=not args.no_focus_click,
        focus_before_each_step=args.focus_each_step,
    )

    print(
        f"Область захвата: left={region.left}, top={region.top}, "
        f"width={region.width}, height={region.height}"
    )
    print(
        f"Прокрутка: метод={scroll.method}, щелчков за шаг={scroll.wheel_notches}, "
        f"микро-шагов={scroll.micro_steps}"
    )
    print("Экстренная остановка: резко переместите мышь в левый верхний угол экрана (pyautogui FAILSAFE).")

    if args.save_frames:
        args.save_frames.mkdir(parents=True, exist_ok=True)

    countdown(
        args.countdown,
        "Переключитесь в окно VRM/браузера со статьёй. Захват начнётся через:",
    )

    frames, reached_end = capture_long_page(
        region,
        scroll,
        scroll_delay=args.scroll_delay,
        settle_delay=args.settle_delay,
        max_frames=args.max_frames,
        same_frame_threshold=args.same_frame_threshold,
        save_frames_dir=args.save_frames,
    )

    if len(frames) < 2 and not reached_end:
        print(
            "Получен только один кадр — прокрутка не сработала. Попробуйте:\n"
            "  --scroll-method win32 --wheel-notches 20 --focus-each-step\n"
            "  или увеличьте --scroll-delay до 0.8",
            file=sys.stderr,
        )
        return 1

    print(f"Склеивание {len(frames)} кадров...")
    expected_overlap = args.expected_overlap
    if expected_overlap <= 0:
        expected_overlap = max(region.height - args.wheel_notches * 30, region.height // 4)
    result = stitch_frames(frames, expected_overlap)

    output = args.output
    if output is None:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output = Path(f"scroll_capture_{stamp}.png")

    result.save(output, format="PNG", optimize=True)
    print(f"Готово: {output.resolve()}")
    print(f"Размер итогового изображения: {result.width} x {result.height} px")
    if not reached_end:
        print("Внимание: лимит кадров исчерпан до конца страницы. Увеличьте --max-frames при необходимости.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
