/* vrmshot.c
 * Автоматический скриншот длинных статей с прокруткой и склейкой.
 *
 * Возможности:
 *  - Выбор прямоугольной области экрана мышью (полупрозрачный оверлей).
 *  - Авто-прокрутка колёсиком мыши внутри выбранной области.
 *  - Адаптивный шаг прокрутки в зависимости от высоты окна.
 *  - Склейка кадров по вертикальной кросс-корреляции (без потери и дублирования строк).
 *  - Детект конца статьи (кадр перестаёт меняться).
 *  - Сохранение результата в PNG.
 *
 * Сборка (MinGW-w64):
 *   gcc -O2 -o vrmshot_v1.exe vrmshot.c -lgdi32 -luser32 -lkernel32 -mwindows
 * Примечание: динамическая линковка обычно даёт меньше ложных AV-срабатываний,
 * чем полностью статический бинарник.
 *
 * Управление:
 *   1) Запустить vrmshot.exe
 *   2) Мышью выделить область (зажать ЛКМ, протянуть, отпустить).
 *   3) Esc — отмена выделения.
 *   4) Программа сама прокручивает и склеивает, результат -> output.png
 */

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ----------------------- Настройки ----------------------- */
#define OVERLAP_RATIO      0.50   /* доля высоты окна на перекрытие (больше = надёжнее склейка таблиц/повторяющихся строк) */
#define MAX_FRAMES         2000   /* предохранитель от бесконечного цикла (хватит на очень длинные страницы) */
#define WHEEL_SETTLE_MS    320    /* пауза после прокрутки, мс (увеличена под плавную/инерционную прокрутку) */
#define END_REPEAT_LIMIT   4      /* сколько почти-одинаковых кадров подряд = конец страницы */
#define MIN_MATCH_ROWS     8      /* минимально осмысленное число строк перекрытия */
#define CONF_MIN           0.06   /* мин. уверенность матча; ниже — доверяем ожидаемому шагу (защита от прыжка на похожую строку) */

/* ----------------------- Структура кадра ----------------------- */
typedef struct {
    int w, h;
    unsigned char *px; /* RGB, 3 байта на пиксель, row-major */
} Frame;

static void frame_free(Frame *f) {
    if (f && f->px) { free(f->px); f->px = NULL; }
}

/* ----------------------- Глобальное состояние выделения ----------------------- */
static RECT g_sel = {0,0,0,0};
static BOOL g_selecting = FALSE;
static BOOL g_done = FALSE;
static BOOL g_cancelled = FALSE;
static POINT g_start;

/* ----------------------- Захват области экрана в RGB ----------------------- */
static int capture_region(int x, int y, int w, int h, Frame *out) {
    HDC hScreen = GetDC(NULL);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    HGDIOBJ old = SelectObject(hMem, hBmp);

    BitBlt(hMem, 0, 0, w, h, hScreen, x, y, SRCCOPY);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;       /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;     /* BGRA */
    bi.bmiHeader.biCompression = BI_RGB;

    unsigned char *bgra = (unsigned char*)malloc((size_t)w * h * 4);
    if (!bgra) goto fail;

    if (!GetDIBits(hMem, hBmp, 0, h, bgra, &bi, DIB_RGB_COLORS)) {
        free(bgra); goto fail;
    }

    out->w = w; out->h = h;
    out->px = (unsigned char*)malloc((size_t)w * h * 3);
    if (!out->px) { free(bgra); goto fail; }

    for (long i = 0, n = (long)w * h; i < n; i++) {
        out->px[i*3+0] = bgra[i*4+2]; /* R */
        out->px[i*3+1] = bgra[i*4+1]; /* G */
        out->px[i*3+2] = bgra[i*4+0]; /* B */
    }
    free(bgra);

    SelectObject(hMem, old);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);
    return 1;

fail:
    SelectObject(hMem, old);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);
    return 0;
}

/* ----------------------- Сигнатура строки (среднее по каналам) ----------------------- */
/* Для скорости сравниваем не пиксели целиком, а профиль строки:
 * для каждой строки считаем массив интенсивностей по столбцам (с шагом),
 * затем меряем разницу строк. Это даёт устойчивый матчинг по тексту. */

static void row_profile(const Frame *f, int row, int *prof, int step, int ncols) {
    const unsigned char *p = f->px + (size_t)row * f->w * 3;
    for (int i = 0; i < ncols; i++) {
        int c = i * step;
        if (c >= f->w) c = f->w - 1;
        const unsigned char *q = p + (size_t)c * 3;
        prof[i] = (q[0] + q[1] + q[2]) / 3;
    }
}

/* Разница двух строковых профилей (сумма абс. разностей). */
static long row_diff(const int *a, const int *b, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += labs(a[i] - b[i]);
    return s;
}

/* ----------------------- Поиск величины прокрутки между кадрами -----------------------
 * Геометрия: общий контент = ВЕРХ нового кадра (cur) совпадает с НИЗОМ предыдущего (prev).
 * Берём шаблон band строк с самого верха cur и ищем, на какой строке y в prev
 * он лучше всего совпадает. Тогда:
 *   d (величина прокрутки в пикселях) = y
 *   overlap (перекрытие) = h - d
 *   новые строки внизу cur = d
 *
 * Ключевые приёмы (проверены тестами на реальном тексте):
 *  - band фиксирован и невелик (помещается в гарантированное перекрытие);
 *  - перебор d по всему диапазону [1 .. h-band];
 *  - мягкий штраф за отклонение от ожидаемого шага hint (разрешает
 *    периодические неоднозначности текста, но даёт концевому кадру
 *    "уехать" к истинному малому сдвигу);
 *  - возвращаем confidence: насколько глобальный минимум выражен.
 *
 * Возвращает d (>=1) или -1 при ошибке. *conf — уверенность (больше = лучше).
 */
static int find_shift(const Frame *prev, const Frame *cur, int hint, double *conf) {
    int ncols = 220;
    int step = cur->w / ncols; if (step < 1) step = 1;
    ncols = cur->w / step; if (ncols < 1) ncols = 1;
    if (ncols > 300) ncols = 300;

    int band = 100;
    if (band > cur->h / 3) band = cur->h / 3;
    if (band < MIN_MATCH_ROWS) band = MIN_MATCH_ROWS;

    int *tmpl = (int*)malloc(sizeof(int) * ncols * band);
    int *line = (int*)malloc(sizeof(int) * ncols);
    if (!tmpl || !line) { free(tmpl); free(line); if (conf) *conf = 0; return -1; }

    for (int r = 0; r < band; r++)
        row_profile(cur, r, tmpl + (size_t)r * ncols, step, ncols);

    int hi = prev->h - band; if (hi < 1) hi = 1;
    double best = -1, second = -1;
    int best_d = -1;

    for (int d = 1; d <= hi; d++) {
        int y = d;
        long acc = 0;
        for (int r = 0; r < band; r++) {
            row_profile(prev, y + r, line, step, ncols);
            acc += row_diff(tmpl + (size_t)r * ncols, line, ncols);
        }
        double score = (double)acc / (double)(band * ncols);
        score += abs(d - hint) * 0.04; /* мягкий приоритет ожидаемого шага */
        if (best < 0 || score < best) { second = best; best = score; best_d = d; }
        else if (second < 0 || score < second) { second = score; }
    }

    free(tmpl); free(line);
    if (conf) *conf = (best <= 0) ? 999.0 : (second - best) / (best + 1.0);
    return best_d;
}

/* Насколько два кадра идентичны целиком (для детекта конца). 0 = идентичны. */
static double frame_similarity_diff(const Frame *a, const Frame *b) {
    if (a->w != b->w || a->h != b->h) return 1e18;
    int ncols = 64;
    int step = a->w / ncols; if (step < 1) step = 1;
    ncols = a->w / step; if (ncols < 1) ncols = 1;
    if (ncols > 256) ncols = 256;

    int *pa = (int*)malloc(sizeof(int)*ncols);
    int *pb = (int*)malloc(sizeof(int)*ncols);
    if (!pa || !pb) { free(pa); free(pb); return 1e18; }

    long total = 0;
    int rows = 0;
    for (int r = 0; r < a->h; r += 4) {
        row_profile(a, r, pa, step, ncols);
        row_profile(b, r, pb, step, ncols);
        total += row_diff(pa, pb, ncols);
        rows++;
    }
    free(pa); free(pb);
    if (rows == 0) return 1e18;
    return (double)total / (double)(rows * ncols);
}

/* ----------------------- Прокрутка колёсиком в точке ----------------------- */
static void scroll_wheel(int cx, int cy, int notches) {
    INPUT in;

    SetCursorPos(cx, cy);
    Sleep(20);
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(notches * -WHEEL_DELTA); /* 120 на одну "ступеньку" */
    SendInput(1, &in, sizeof(in));
}

/* ----------------------- Оверлей для выбора области ----------------------- */
static LRESULT CALLBACK OverlayProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            g_selecting = TRUE;
            g_start.x = GET_X_LPARAM(lp);
            g_start.y = GET_Y_LPARAM(lp);
            g_sel.left = g_sel.right = g_start.x;
            g_sel.top = g_sel.bottom = g_start.y;
            SetCapture(hWnd);
            return 0;
        case WM_MOUSEMOVE:
            if (g_selecting) {
                int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                g_sel.left   = min(g_start.x, x);
                g_sel.top    = min(g_start.y, y);
                g_sel.right  = max(g_start.x, x);
                g_sel.bottom = max(g_start.y, y);
                InvalidateRect(hWnd, NULL, TRUE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_selecting) {
                g_selecting = FALSE;
                ReleaseCapture();
                g_done = TRUE;
                DestroyWindow(hWnd);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                g_cancelled = TRUE;
                DestroyWindow(hWnd);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT full; GetClientRect(hWnd, &full);
            /* затемнение всего экрана */
            HBRUSH dark = CreateSolidBrush(RGB(0,0,0));
            FillRect(hdc, &full, dark);
            DeleteObject(dark);
            /* рамка выделения */
            if (g_sel.right > g_sel.left && g_sel.bottom > g_sel.top) {
                HBRUSH clear = CreateSolidBrush(RGB(40,40,40));
                FillRect(hdc, &g_sel, clear);
                DeleteObject(clear);
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(0,200,255));
                HGDIOBJ oldp = SelectObject(hdc, pen);
                HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, g_sel.left, g_sel.top, g_sel.right, g_sel.bottom);
                SelectObject(hdc, oldp); SelectObject(hdc, oldb);
                DeleteObject(pen);
                char buf[128];
                sprintf(buf, "%ld x %ld", g_sel.right-g_sel.left, g_sel.bottom-g_sel.top);
                SetTextColor(hdc, RGB(0,200,255));
                SetBkMode(hdc, TRANSPARENT);
                TextOutA(hdc, g_sel.left+4, g_sel.top-20, buf, (int)strlen(buf));
            }
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

static int select_region(RECT *result) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "VrmShotOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    RegisterClassA(&wc);

    int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HWND hWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED,
        "VrmShotOverlay", "", WS_POPUP,
        sx, sy, sw, sh, NULL, NULL, hInst, NULL);

    SetLayeredWindowAttributes(hWnd, 0, 120, LWA_ALPHA); /* полупрозрачно */
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (g_done || g_cancelled) break;
    }
    /* добиваем очередь до уничтожения окна */
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_cancelled) return 0;

    /* перевод в координаты виртуального экрана */
    result->left   = g_sel.left   + sx;
    result->top    = g_sel.top    + sy;
    result->right  = g_sel.right  + sx;
    result->bottom = g_sel.bottom + sy;
    return (result->right - result->left > 10 && result->bottom - result->top > 10);
}

/* ----------------------- Накопитель итоговой картинки ----------------------- */
typedef struct {
    int w, h, cap;
    unsigned char *px; /* RGB */
} Canvas;

static int canvas_append(Canvas *c, const unsigned char *src, int w, int rows, int src_row_start) {
    if (c->w == 0) c->w = w;
    if (w != c->w) return 0;
    long need = (long)(c->h + rows) * c->w * 3;
    if (need > c->cap) {
        long ncap = need * 2;
        unsigned char *np = (unsigned char*)realloc(c->px, ncap);
        if (!np) return 0;
        c->px = np; c->cap = (int)ncap;
    }
    memcpy(c->px + (size_t)c->h * c->w * 3,
           src + (size_t)src_row_start * w * 3,
           (size_t)rows * w * 3);
    c->h += rows;
    return 1;
}

/* ----------------------- main ----------------------- */
int WINAPI WinMain(HINSTANCE hI, HINSTANCE hP, LPSTR lpCmd, int nShow) {
    (void)hI;(void)hP;(void)lpCmd;(void)nShow;

    RECT r;
    if (!select_region(&r)) {
        MessageBoxA(NULL, "Выделение отменено или область слишком мала.", "vrmshot", MB_OK);
        return 0;
    }

    int x = r.left, y = r.top;
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    /* дать оверлею исчезнуть и пользователю отвести курсор */
    Sleep(400);

    int cx = x + w/2, cy = y + h/2;

    /* Адаптивный шаг прокрутки.
     * ВАЖНО (доказано тестами): прокрутка должна оставлять БОЛЬШОЕ гарантированное
     * перекрытие, иначе на повторяющихся структурах (таблицы, списки с одинаковой
     * высотой строк) шаблон склейки цепляется за соседнюю похожую строку и
     * "проглатывает" данные. Поэтому шаг = (1 - OVERLAP_RATIO) высоты, но НЕ больше
     * безопасного максимума ~0.52*h.
     * Одна "ступенька" колеса = неизвестно сколько пикселей (зависит от
     * приложения), поэтому стартуем с оценки и калибруем по факту. */
    int safe_max_scroll = (int)(h * 0.52);
    int target_scroll = (int)(h * (1.0 - OVERLAP_RATIO));
    if (target_scroll > safe_max_scroll) target_scroll = safe_max_scroll;
    if (target_scroll < 20) target_scroll = 20;

    int px_per_notch = 100;          /* стартовая оценка пикселей на ступеньку */
    int notches = target_scroll / px_per_notch;
    if (notches < 1) notches = 1;

    int target_d = target_scroll;    /* ожидаемый сдвиг для подсказки матчингу */

    Canvas canvas; memset(&canvas, 0, sizeof(canvas));

    Frame prev; memset(&prev, 0, sizeof(prev));
    if (!capture_region(x, y, w, h, &prev)) {
        MessageBoxA(NULL, "Не удалось захватить экран.", "vrmshot", MB_OK);
        return 1;
    }
    /* первый кадр кладём целиком */
    canvas_append(&canvas, prev.px, prev.w, prev.h, 0);

    int end_repeat = 0;
    int frames = 1;

    for (; frames < MAX_FRAMES; frames++) {
        scroll_wheel(cx, cy, notches);
        Sleep(WHEEL_SETTLE_MS);

        Frame cur; memset(&cur, 0, sizeof(cur));
        if (!capture_region(x, y, w, h, &cur)) break;

        /* ищем величину прокрутки d (новые строки внизу cur) */
        double conf;
        int d = find_shift(&prev, &cur, target_d, &conf);

        /* Защита от "прыжка" на похожую строку (типично для таблиц):
         * если уверенность матча низкая, не доверяем ему и берём ожидаемый
         * (стабильный) шаг прокрутки. Это предотвращает потерю данных. */
        int trust = (conf >= CONF_MIN && d >= 1);
        if (!trust) d = target_d;
        if (d > h) d = h;
        if (d < 1) d = 1;

        /* Детект конца страницы: страница практически перестала прокручиваться.
         * Конец засчитываем при УВЕРЕННОМ совпадении с очень малым сдвигом
         * (упёрлись в низ) ЛИБО при почти полной идентичности кадров (статичный
         * футер/пустота). Несколько таких подряд = достигли низа. */
        double sim = frame_similarity_diff(&prev, &cur);
        int hit_bottom = (trust && d <= 3);          /* упор прокрутки */
        int static_frame = (sim < 1.2);              /* кадр почти не меняется */
        if (hit_bottom || static_frame) {
            /* при реальном упоре дописываем небольшой найденный остаток;
             * статичные (почти пустые/неизменные) кадры НЕ дописываем,
             * чтобы внизу не копился лишний белый/повторяющийся хвост */
            if (hit_bottom && !static_frame && d >= 1 && d <= h)
                canvas_append(&canvas, cur.px, cur.w, d, h - d);
            end_repeat++;
            frame_free(&cur);
            if (end_repeat >= END_REPEAT_LIMIT) break;
            notches += 1; /* вдруг прокрутка "залипла" — толкнём сильнее */
            continue;
        } else {
            end_repeat = 0;
        }

        int new_rows = d;

        /* Калибровка колеса: фактически прокрутилось d пикселей за notches ступенек.
         * Обновляем оценку px_per_notch (сглаженно) и пересчитываем notches так,
         * чтобы следующий шаг попадал в target_scroll. */
        if (d > 4 && notches > 0) {
            int measured = d / notches;
            if (measured > 2) {
                px_per_notch = (px_per_notch * 2 + measured) / 3;
                if (px_per_notch < 1) px_per_notch = 1;
                int nn = target_scroll / px_per_notch;
                if (nn < 1) nn = 1;
                notches = nn;
            }
        }
        /* сглаженно обновляем подсказку ожидаемого сдвига; но не выше запрошенного
         * шага, чтобы единичный сбой не "разгонял" подсказку и не ломал склейку */
        target_d = (target_d * 3 + d) / 4;
        if (target_d > target_scroll) target_d = target_scroll;
        if (target_d < 8) target_d = 8;

        /* добавляем только новые строки внизу нового кадра: cur[h-d .. h] */
        canvas_append(&canvas, cur.px, cur.w, new_rows, h - new_rows);

        frame_free(&prev);
        prev = cur; /* передаём владение */
    }

    frame_free(&prev);

    if (canvas.h == 0 || !canvas.px) {
        MessageBoxA(NULL, "Не удалось собрать изображение.", "vrmshot", MB_OK);
        return 1;
    }

    /* формируем имя файла с датой и временем: output_dd.mm.yy_hh-mm-ss.png
     * (двоеточие в именах файлов Windows запрещено, поэтому время через дефис) */
    SYSTEMTIME st;
    GetLocalTime(&st);
    char fname[64];
    sprintf(fname, "output_%02d.%02d.%02d_%02d-%02d-%02d.png",
            st.wDay, st.wMonth, st.wYear % 100,
            st.wHour, st.wMinute, st.wSecond);

    /* сохраняем PNG */
    int ok = stbi_write_png(fname, canvas.w, canvas.h, 3,
                            canvas.px, canvas.w * 3);

    char msg[256];
    if (ok) {
        sprintf(msg, "Готово!\nКадров: %d\nИтог: %d x %d px\nФайл: %s",
                frames, canvas.w, canvas.h, fname);
    } else {
        sprintf(msg, "Ошибка записи PNG.");
    }
    MessageBoxA(NULL, msg, "vrmshot", MB_OK);

    free(canvas.px);
    return ok ? 0 : 1;
}
