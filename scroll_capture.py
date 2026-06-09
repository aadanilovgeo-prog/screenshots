#!/usr/bin/env python3
"""
Автоматический захват длинной страницы: прокрутка мышью, скриншоты, склейка в один файл.

Предназначено для сценариев вроде VRM/удалённого рабочего стола, когда нельзя
скопировать текст или выгрузить файл, но можно просматривать страницу на экране.
"""

from __future__ import annotations

__version__ = "1.2.1"

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
PX_PER_WHEEL_NOTCH = 35
HEADER_SKIP_FRAC = 0.12
SEAM_GOOD = 0.045
SEAM_BAD = 0.075


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


@dataclass
class OverlapMatch:
    overlap: int
    score: float


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


def _ncc(a: np.ndarray, b: np.ndarray) -> float:
    if len(a) < 2:
        return 0.0
    a = a - float(a.mean())
    b = b - float(b.mean())
    denom = float(np.sqrt(np.sum(a * a) * np.sum(b * b))) + 1e-6
    return float(np.sum(a * b) / denom)


def _prepare_match_arrays(img: Image.Image) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    gray = np.asarray(img.convert("L"), dtype=np.float32)
    height, width = gray.shape
    margin_x = max(8, int(width * 0.12))
    gray = gray[:, margin_x : width - margin_x]
    gradient = np.abs(np.diff(gray, axis=0))
    gradient = np.vstack([gradient, gradient[-1:]])
    intensity_profile = gray.mean(axis=1)
    gradient_profile = gradient.mean(axis=1)
    return gray, intensity_profile, gradient_profile


def overlap_search_bounds(
    frame_height: int,
    wheel_notches: int,
    expected_overlap: int = 0,
    preferred_overlap: int | None = None,
) -> tuple[int, int]:
    estimated_scroll = max(40, wheel_notches * PX_PER_WHEEL_NOTCH)
    if preferred_overlap is not None:
        min_overlap = max(40, preferred_overlap - 22)
        max_overlap = min(frame_height - 30, preferred_overlap + 18)
    elif expected_overlap > 0:
        min_overlap = max(40, expected_overlap - 70)
        max_overlap = min(frame_height - 30, expected_overlap + 35)
    else:
        min_scroll = int(estimated_scroll * 0.82)
        max_scroll = int(estimated_scroll * 1.08)
        min_overlap = max(40, frame_height - max_scroll)
        max_overlap = min(frame_height - 30, frame_height - min_scroll)
    if min_overlap >= max_overlap:
        min_overlap, max_overlap = 40, frame_height - 40
    return min_overlap, max_overlap




def _seam_mean_diff(img_above: Image.Image, img_below: Image.Image, overlap: int) -> float:
    import numpy as np
    a = np.asarray(img_above, dtype=np.int16)
    b = np.asarray(img_below, dtype=np.int16)
    skip = int(overlap * HEADER_SKIP_FRAC)
    if overlap - skip < 8:
        skip = 0
    region_a = a[a.shape[0] - overlap + skip : a.shape[0] : 2, :, :]
    region_b = b[skip:overlap:2, :, :]
    if region_a.size == 0:
        return 1.0
    return float(np.mean(np.abs(region_a - region_b)) / 255.0)


def _refine_overlap_by_seam(
    img_above: Image.Image,
    img_below: Image.Image,
    overlap: int,
    min_overlap: int,
    max_overlap: int,
    expected_overlap: int,
) -> int:
    seam = _seam_mean_diff(img_above, img_below, overlap)
    best = overlap
    if seam <= SEAM_GOOD:
        return overlap
    for delta in range(2, 52, 2):
        up = overlap + delta
        if up <= max_overlap:
            s = _seam_mean_diff(img_above, img_below, up)
            if s < seam:
                seam, best = s, up
        if seam <= SEAM_GOOD:
            return best
    if seam > SEAM_BAD:
        for delta in range(2, 32, 2):
            down = overlap - delta
            if down >= min_overlap:
                s = _seam_mean_diff(img_above, img_below, down)
                if s < seam:
                    seam, best = s, down
            if seam <= SEAM_GOOD:
                return best
        if min_overlap <= expected_overlap <= max_overlap:
            s = _seam_mean_diff(img_above, img_below, expected_overlap)
            if s < seam:
                best = expected_overlap
    if seam > SEAM_BAD:
        print(f"  [warn] weak seam match ({seam:.3f}), overlap={best}px")
    return best

def _strip_ssd(gray_above: np.ndarray, gray_below: np.ndarray, overlap: int, x0: int, x1: int, skip_top: int = 0) -> float:
    patch_above = gray_above[gray_above.shape[0] - overlap + skip_top : gray_above.shape[0], x0:x1]
    patch_below = gray_below[skip_top:overlap, x0:x1]
    return float(np.mean(np.abs(patch_above - patch_below)))


def _overlap_cost(
    gray_above: np.ndarray,
    gray_below: np.ndarray,
    prof_above: np.ndarray,
    prof_below: np.ndarray,
    gprof_above: np.ndarray,
    gprof_below: np.ndarray,
    overlap: int,
    preferred_overlap: int | None = None,
) -> float:
    ha = gray_above.shape[0]
    w = gray_above.shape[1]
    s1, s2 = w // 4, w // 2
    skip_top = max(6, int(ha * HEADER_SKIP_FRAC))
    ssd = (
        _strip_ssd(gray_above, gray_below, overlap, 0, s1, skip_top)
        + _strip_ssd(gray_above, gray_below, overlap, s1, s2, skip_top)
        + _strip_ssd(gray_above, gray_below, overlap, s2, w, skip_top)
    ) / 3.0

    ncc_int = _ncc(prof_above[ha - overlap : ha], prof_below[:overlap])
    ncc_grad = _ncc(gprof_above[ha - overlap : ha], gprof_below[:overlap])

    cost = ssd - 45.0 * ncc_int - 25.0 * ncc_grad
    cost += 0.04 * overlap
    if preferred_overlap is not None:
        if overlap > preferred_overlap:
            cost += 0.25 * (overlap - preferred_overlap)
        else:
            cost += 0.12 * (preferred_overlap - overlap)
    return cost


def find_vertical_overlap(
    img_above: Image.Image,
    img_below: Image.Image,
    *,
    min_overlap: int,
    max_overlap: int,
    preferred_overlap: int | None = None,
) -> OverlapMatch:
    gray_above, prof_above, gprof_above = _prepare_match_arrays(img_above)
    gray_below, prof_below, gprof_below = _prepare_match_arrays(img_below)

    height = min(gray_above.shape[0], gray_below.shape[0])
    max_overlap = min(max_overlap, height - 20, gray_above.shape[0] - 10, gray_below.shape[0] - 10)
    min_overlap = max(20, min_overlap)

    if min_overlap >= max_overlap:
        fallback = max(20, min(min_overlap, max_overlap))
        return OverlapMatch(overlap=fallback, score=1.0)

    best_overlap = min_overlap
    best_cost = float("inf")

    for overlap in range(min_overlap, max_overlap + 1, 2):
        cost = _overlap_cost(
            gray_above,
            gray_below,
            prof_above,
            prof_below,
            gprof_above,
            gprof_below,
            overlap,
            preferred_overlap,
        )
        if cost < best_cost:
            best_cost = cost
            best_overlap = overlap

    fine_min = max(min_overlap, best_overlap - 8)
    fine_max = min(max_overlap, best_overlap + 8)
    for overlap in range(fine_min, fine_max + 1):
        cost = _overlap_cost(
            gray_above,
            gray_below,
            prof_above,
            prof_below,
            gprof_above,
            gprof_below,
            overlap,
            preferred_overlap,
        )
        if cost < best_cost:
            best_cost = cost
            best_overlap = overlap

    expected = preferred_overlap if preferred_overlap is not None else (min_overlap + max_overlap) // 2
    best_overlap = _refine_overlap_by_seam(img_above, img_below, best_overlap, min_overlap, max_overlap, expected)
    return OverlapMatch(overlap=best_overlap, score=best_cost)


def stabilize_overlaps(overlaps: list[int], scores: list[float]) -> list[int]:
    if len(overlaps) <= 2:
        return overlaps

    median_overlap = int(np.median(overlaps))
    stabilized: list[int] = []
    for overlap, score in zip(overlaps, scores):
        if abs(overlap - median_overlap) > 50:
            stabilized.append(median_overlap)
        elif overlap > median_overlap + 20:
            stabilized.append(median_overlap + 8)
        elif overlap < median_overlap - 20:
            stabilized.append(median_overlap - 8)
        else:
            stabilized.append(overlap)

    smoothed: list[int] = []
    for index in range(len(stabilized)):
        start = max(0, index - 1)
        end = min(len(stabilized), index + 2)
        window = sorted(stabilized[start:end])
        smoothed.append(window[len(window) // 2])
    return smoothed


def stitch_frames(frames: list[Image.Image], overlaps: list[int]) -> Image.Image:
    if not frames:
        raise ValueError("Нет кадров для склейки")
    if len(frames) == 1:
        return frames[0].copy()

    if len(overlaps) != len(frames) - 1:
        raise ValueError("Число перекрытий должно быть на 1 меньше числа кадров")

    pieces: list[Image.Image] = [frames[0]]
    for index in range(1, len(frames)):
        overlap = max(1, min(overlaps[index - 1], frames[index].height - 1))
        pieces.append(frames[index].crop((0, overlap, frames[index].width, frames[index].height)))

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

    if settings.focus_before_each_step:
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
    expected_overlap: int,
    save_frames_dir: Path | None,
) -> tuple[list[Image.Image], list[int], bool]:
    frames: list[Image.Image] = []
    overlaps: list[int] = []
    overlap_scores: list[float] = []
    reached_end = False

    preferred_overlap: int | None = expected_overlap if expected_overlap > 0 else None

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
            min_overlap, max_overlap = overlap_search_bounds(
                region.height,
                scroll.wheel_notches,
                expected_overlap,
                preferred_overlap,
            )
            match = find_vertical_overlap(
                previous,
                current,
                min_overlap=min_overlap,
                max_overlap=max_overlap,
                preferred_overlap=preferred_overlap,
            )
            preferred_overlap = match.overlap
            overlaps.append(match.overlap)
            overlap_scores.append(match.score)
            print(
                f"  Кадр {index:04d}: отличие {diff * 100:.2f}%, "
                f"перекрытие {match.overlap}px"
            )

            if diff <= same_frame_threshold:
                print("Страница, похоже, достигла конца (кадры совпадают).")
                reached_end = True
                overlaps.pop()
                overlap_scores.pop()
                break

            frames.append(current.copy())
            if save_frames_dir:
                current.save(save_frames_dir / f"frame_{index:04d}.png")
            previous = current

    overlaps = stabilize_overlaps(overlaps, overlap_scores)
    return frames, overlaps, reached_end


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
        default=8,
        help="Сколько «щелчков» колёсика за один шаг (меньше = больше перекрытие, аккуратнее склейка)",
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
        help="Кликать в область перед каждым шагом (по умолчанию клик только при старте)",
    )
    parser.add_argument(
        "--scroll-delay",
        type=float,
        default=0.8,
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
        default=600,
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

    if "--version" in sys.argv or "-V" in sys.argv:
        print(f"scroll_capture {__version__}")
        return 0

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

    expected_overlap = args.expected_overlap
    if expected_overlap <= 0:
        estimated_scroll = max(40, args.wheel_notches * PX_PER_WHEEL_NOTCH)
        expected_overlap = max(region.height - estimated_scroll, region.height // 3)

    print(f"scroll_capture v{__version__}")
    print(
        f"Область захвата: left={region.left}, top={region.top}, "
        f"width={region.width}, height={region.height}"
    )
    print(
        f"Прокрутка: метод={scroll.method}, щелчков за шаг={scroll.wheel_notches}, "
        f"микро-шагов={scroll.micro_steps}, ожидаемое перекрытие ~{expected_overlap}px"
    )
    print("Экстренная остановка: резко переместите мышь в левый верхний угол экрана (pyautogui FAILSAFE).")

    if args.save_frames:
        args.save_frames.mkdir(parents=True, exist_ok=True)

    countdown(
        args.countdown,
        "Переключитесь в окно VRM/браузера со статьёй. Захват начнётся через:",
    )

    frames, overlaps, reached_end = capture_long_page(
        region,
        scroll,
        scroll_delay=args.scroll_delay,
        settle_delay=args.settle_delay,
        max_frames=args.max_frames,
        same_frame_threshold=args.same_frame_threshold,
        expected_overlap=expected_overlap,
        save_frames_dir=args.save_frames,
    )

    if len(frames) < 2 and not reached_end:
        print(
            "Получен только один кадр — прокрутка не сработала. Попробуйте:\n"
            "  --scroll-method win32 --wheel-notches 20 --focus-each-step\n"
            "  или увеличьте --scroll-delay",
            file=sys.stderr,
        )
        return 1

    print(f"Склеивание {len(frames)} кадров...")
    if overlaps:
        print(
            "  Перекрытия после стабилизации: "
            f"min={min(overlaps)}, max={max(overlaps)}, median={int(np.median(overlaps))} px"
        )
    result = stitch_frames(frames, overlaps)

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
