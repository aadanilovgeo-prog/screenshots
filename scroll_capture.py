#!/usr/bin/env python3
"""
Автоматический захват длинной страницы: прокрутка мышью, скриншоты, склейка в один файл.

Предназначено для сценариев вроде VRM/удалённого рабочего стола, когда нельзя
скопировать текст или выгрузить файл, но можно просматривать страницу на экране.
"""

from __future__ import annotations

import argparse
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
pyautogui.PAUSE = 0.05


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


def parse_region(value: str) -> Region:
    parts = [int(part.strip()) for part in value.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("Формат области: left,top,width,height")
    left, top, width, height = parts
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("Ширина и высота должны быть больше нуля")
    return Region(left, top, width, height)


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
    """Подбирает высоту перекрытия между нижней частью верхнего и верхней частью нижнего кадра."""
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


def scroll_at(region: Region, scroll_clicks: int) -> None:
    pyautogui.moveTo(*region.center, duration=0.15)
    pyautogui.scroll(-scroll_clicks)


def capture_long_page(
    region: Region,
    *,
    scroll_clicks: int,
    scroll_delay: float,
    settle_delay: float,
    max_frames: int,
    same_frame_threshold: float,
    save_frames_dir: Path | None,
) -> tuple[list[Image.Image], bool]:
    frames: list[Image.Image] = []
    reached_end = False

    with mss.mss() as sct:
        print("Захват первого кадра...")
        previous = grab_region(sct, region)
        frames.append(previous.copy())
        if save_frames_dir:
            previous.save(save_frames_dir / "frame_0000.png")

        for index in range(1, max_frames + 1):
            scroll_at(region, scroll_clicks)
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
        "--scroll-clicks",
        type=int,
        default=8,
        help="Интенсивность прокрутки колёсиком за один шаг (больше = дальше листает)",
    )
    parser.add_argument(
        "--scroll-delay",
        type=float,
        default=0.35,
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


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    region = args.region or pick_region_interactive()
    print(
        f"Область захвата: left={region.left}, top={region.top}, "
        f"width={region.width}, height={region.height}"
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
        scroll_clicks=args.scroll_clicks,
        scroll_delay=args.scroll_delay,
        settle_delay=args.settle_delay,
        max_frames=args.max_frames,
        same_frame_threshold=args.same_frame_threshold,
        save_frames_dir=args.save_frames,
    )

    if len(frames) < 2 and not reached_end:
        print("Получен только один кадр. Попробуйте увеличить --scroll-clicks.", file=sys.stderr)
        return 1

    print(f"Склеивание {len(frames)} кадров...")
    expected_overlap = args.expected_overlap
    if expected_overlap <= 0:
        expected_overlap = max(region.height - args.scroll_clicks * 40, region.height // 4)
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
