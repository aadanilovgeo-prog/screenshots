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
 *   gcc -O2 -o vrmshot_v7.exe vrmshot.c -lgdi32 -luser32 -lkernel32 -mwindows
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
#include <stdarg.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ----------------------- Настройки ----------------------- */
#define OVERLAP_RATIO      0.56   /* доля высоты окна на перекрытие (больше = надёжнее склейка) */
#define MAX_FRAMES         2000   /* предохранитель от бесконечного цикла (хватит на очень длинные страницы) */
#define WHEEL_SETTLE_MS    600    /* пауза после прокрутки, мс (увеличена под плавную/инерционную прокрутку) */
#define END_REPEAT_LIMIT   3      /* сколько кадров без прогресса подряд = конец страницы */
#define MIN_MATCH_ROWS     8      /* минимально осмысленное число строк перекрытия */
#define CONF_MIN           0.06   /* мин. уверенность матча; ниже — доверяем ожидаемому шагу */
#define SHIFT_WINDOW_FRAC       0.28  /* поиск сдвига вокруг ожидаемого шага (±28%) */
#define SHIFT_WINDOW_FRAC_TIGHT 0.15  /* суженное окно после плохих швов */
#define SHIFT_WINDOW_BAD_LIMIT  2     /* подряд плохих швов до сужения окна */
#define NO_PROGRESS_DIFF        2.5   /* порог "нет нового контента" */
#define SEAM_MAX_DIFF           4.0   /* макс. допустимое расхождение на шве overlap */
#define SEAM_GOOD               2.8   /* хороший шов — можно доверять сдвигу */
#define MAX_SHIFT_OVER_FRAC     0.06  /* макс. перелёт относительно target_d (+6%) */
#define CONF_HIGH               0.14  /* высокая уверенность для увеличения d */
#define SAME_FRAME_DIFF         2.0   /* кадр почти не изменился после скролла */
#define MAX_CANVAS_HEIGHT       600000
#define STABLE_ATTEMPTS         3
#define STABLE_INTERVAL_MS      150
#define STABLE_MAX_DIFF         2.0
#define MICRO_NOTCH_DELAY_MS    60
#define FOCUS_DELAY_MS          100
#define ROW_PROFILE_MARGIN_FRAC 0.14
#define LOG_FILE_NAME           "vrmshot_log.txt"

static FILE *g_log = NULL;
static int g_bad_seam_streak = 0;

static void log_open(void) {
    g_log = fopen(LOG_FILE_NAME, "w");
    if (g_log) {
        fprintf(g_log, "vrmshot log\n");
        fflush(g_log);
    }
}

static void log_close(void) {
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

static void log_msg(const char *fmt, ...) {
    va_list ap;
    if (!g_log) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static double current_shift_window_frac(void) {
    if (g_bad_seam_streak >= SHIFT_WINDOW_BAD_LIMIT) {
        return SHIFT_WINDOW_FRAC_TIGHT;
    }
    return SHIFT_WINDOW_FRAC;
}

/* ----------------------- Структура кадра ----------------------- */
typedef struct {
    int w, h;
    unsigned char *px; /* RGB, 3 байта на пиксель, row-major */
} Frame;

static double strip_mean_diff(const Frame *a, int a_row, const Frame *b, int b_row, int rows);

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

static int capture_region_stable(int x, int y, int w, int h, Frame *out) {
    Frame a;
    Frame b;
    int attempt;
    int check_rows;
    double diff;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    if (!capture_region(x, y, w, h, &b)) {
        return 0;
    }

    check_rows = h / 6;
    if (check_rows < 12) {
        check_rows = 12;
    }
    if (check_rows > 48) {
        check_rows = 48;
    }

    for (attempt = 0; attempt < STABLE_ATTEMPTS; attempt++) {
        Sleep(STABLE_INTERVAL_MS);
        if (!capture_region(x, y, w, h, &a)) {
            break;
        }
        diff = strip_mean_diff(&b, b.h / 3, &a, a.h / 3, check_rows);
        if (diff < STABLE_MAX_DIFF) {
            frame_free(&b);
            *out = a;
            return 1;
        }
        frame_free(&b);
        b = a;
        a.px = NULL;
    }

    *out = b;
    return 1;
}

/* ----------------------- Сигнатура строки (среднее по каналам) ----------------------- */
/* Для скорости сравниваем не пиксели целиком, а профиль строки:
 * для каждой строки считаем массив интенсивностей по столбцам (с шагом),
 * затем меряем разницу строк. Это даёт устойчивый матчинг по тексту. */

static void row_profile(const Frame *f, int row, int *prof, int step, int ncols) {
    const unsigned char *p = f->px + (size_t)row * f->w * 3;
    int margin = (int)(f->w * ROW_PROFILE_MARGIN_FRAC);
    int x0 = margin;
    int x1 = f->w - margin;
    int usable = x1 - x0;
    int i;

    if (usable < 8) {
        x0 = 0;
        x1 = f->w;
        usable = f->w;
    }

    for (i = 0; i < ncols; i++) {
        int c = x0 + (i * usable) / (ncols > 1 ? (ncols - 1) : 1);
        if (c >= f->w) {
            c = f->w - 1;
        }
        if (c < 0) {
            c = 0;
        }
        {
            const unsigned char *q = p + (size_t)c * 3;
            prof[i] = (q[0] + q[1] + q[2]) / 3;
        }
        (void)step;
    }
}

/* Разница двух строковых профилей (сумма абс. разностей). */
static long row_diff(const int *a, const int *b, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += labs(a[i] - b[i]);
    return s;
}

static int match_profile_cols(const Frame *f, int *step_out, int *ncols_out) {
    int ncols = 220;
    int step = f->w / ncols;
    if (step < 1) step = 1;
    ncols = f->w / step;
    if (ncols < 1) ncols = 1;
    if (ncols > 300) ncols = 300;
    *step_out = step;
    *ncols_out = ncols;
    return 1;
}

static int match_band_rows(const Frame *f) {
    int band = 80;
    if (band > f->h / 4) band = f->h / 4;
    if (band < MIN_MATCH_ROWS) band = MIN_MATCH_ROWS;
    return band;
}

static double shift_match_band_score(
    const Frame *prev,
    const Frame *cur,
    int prev_start,
    int cur_start,
    int band,
    int step,
    int ncols,
    int *tmpl,
    int *line
) {
    long acc = 0;
    int r;

    if (prev_start < 0 || cur_start < 0 || prev_start + band > prev->h || cur_start + band > cur->h) {
        return 1e18;
    }

    for (r = 0; r < band; r++) {
        row_profile(cur, cur_start + r, tmpl + (size_t)r * ncols, step, ncols);
    }
    for (r = 0; r < band; r++) {
        row_profile(prev, prev_start + r, line, step, ncols);
        acc += row_diff(tmpl + (size_t)r * ncols, line, ncols);
    }

    return (double)acc / (double)(band * ncols);
    (void)step;
}

/* Оценка совпадения: верх cur совпадает с prev, начиная со строки d (два band-шаблона). */
static double shift_match_score(
    const Frame *prev,
    const Frame *cur,
    int d,
    int band,
    int step,
    int ncols,
    int *tmpl,
    int *line
) {
    double s1;
    double s2;

    if (d < 1 || d + band > prev->h) {
        return 1e18;
    }

    s1 = shift_match_band_score(prev, cur, d, 0, band, step, ncols, tmpl, line);
    if (band * 2 <= cur->h && d + band * 2 <= prev->h) {
        s2 = shift_match_band_score(prev, cur, d + band, band, band, step, ncols, tmpl, line);
        return (s1 + s2) * 0.5;
    }
    return s1;
}

/* Средняя разница между двумя полосами строк одного кадра. */
static double strip_mean_diff(const Frame *a, int a_row, const Frame *b, int b_row, int rows) {
    int step, ncols;
    int *pa;
    int *pb;
    long total = 0;
    int count = 0;

    if (!a || !b || rows < 1) {
        return 1e18;
    }
    if (a_row < 0 || b_row < 0 || a_row + rows > a->h || b_row + rows > b->h) {
        return 1e18;
    }

    match_profile_cols(a, &step, &ncols);
    pa = (int*)malloc(sizeof(int) * (size_t)ncols);
    pb = (int*)malloc(sizeof(int) * (size_t)ncols);
    if (!pa || !pb) {
        free(pa);
        free(pb);
        return 1e18;
    }

    for (int r = 0; r < rows; r += 2) {
        row_profile(a, a_row + r, pa, step, ncols);
        row_profile(b, b_row + r, pb, step, ncols);
        total += row_diff(pa, pb, ncols);
        count++;
    }

    free(pa);
    free(pb);
    if (count == 0) {
        return 1e18;
    }
    return (double)total / (double)(count * ncols);
}

/* Поиск сдвига только в окне вокруг hint + отдельная проверка малого сдвига у низа страницы. */
static int find_shift(const Frame *prev, const Frame *cur, int hint, double *conf) {
    int step, ncols;
    int band;
    int hi;
    int lo;
    int hi_win;
    int best_d = -1;
    int small_d = -1;
    double best = -1;
    double second = -1;
    double small_best = -1;
    double hint_score;
    int *tmpl;
    int *line;

    if (!prev || !cur || hint < 1) {
        if (conf) *conf = 0;
        return -1;
    }

    match_profile_cols(cur, &step, &ncols);
    band = match_band_rows(cur);
    hi = prev->h - band;
    if (hi < 1) hi = 1;

    tmpl = (int*)malloc(sizeof(int) * (size_t)ncols * (size_t)band);
    line = (int*)malloc(sizeof(int) * (size_t)ncols);
    if (!tmpl || !line) {
        free(tmpl);
        free(line);
        if (conf) *conf = 0;
        return -1;
    }

    {
        double window_frac = current_shift_window_frac();
        lo = hint - (int)(hint * window_frac);
        hi_win = hint + (int)(hint * window_frac);
    }
    if (lo < 1) lo = 1;
    if (hi_win > hi) hi_win = hi;

    for (int d = 1; d <= hi; d++) {
        double score;
        int in_window = (d >= lo && d <= hi_win);
        int is_small = (d <= 12);

        if (!in_window && !is_small) {
            continue;
        }

        score = shift_match_score(prev, cur, d, band, step, ncols, tmpl, line);
        if (is_small && (small_best < 0 || score < small_best)) {
            small_best = score;
            small_d = d;
        }
        if (!in_window) {
            continue;
        }

        score += abs(d - hint) * 0.05;
        if (best < 0 || score < best) {
            second = best;
            best = score;
            best_d = d;
        } else if (second < 0 || score < second) {
            second = score;
        }
    }

    hint_score = shift_match_score(prev, cur, hint, band, step, ncols, tmpl, line);
    if (best_d < 0) {
        best_d = hint;
        best = hint_score;
        second = -1;
    } else if (hint_score <= best * 1.08) {
        best_d = hint;
        best = hint_score;
    }

    if (small_d > 0 && small_best <= best * 0.92 && small_d <= 8) {
        best_d = small_d;
        best = small_best;
    }

    free(tmpl);
    free(line);

    if (conf) {
        *conf = (best <= 0) ? 999.0 : (second - best) / (best + 1.0);
    }
    return best_d;
}

/* Один раз в начале захвата: фокус окна без повторных кликов при скролле. */
static void focus_point_once(int cx, int cy) {
    INPUT in[2];

    SetCursorPos(cx, cy);
    Sleep(20);
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
    Sleep(FOCUS_DELAY_MS);
}

/* Прокрутка без клика — только позиция курсора и колесо. */
static void scroll_wheel(int cx, int cy, int notches) {
    int i;

    SetCursorPos(cx, cy);
    Sleep(20);
    for (i = 0; i < notches; i++) {
        INPUT in;
        memset(&in, 0, sizeof(in));
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)(-WHEEL_DELTA);
        SendInput(1, &in, sizeof(in));
        if (i + 1 < notches) {
            Sleep(MICRO_NOTCH_DELAY_MS);
        }
    }
}

static double frame_quick_diff(const Frame *a, const Frame *b) {
    int rows = 16;
    double d1;
    double d2;
    double d3;

    if (!a || !b || a->h < rows * 3 || b->h < rows * 3) {
        return 1e18;
    }

    d1 = strip_mean_diff(a, a->h / 4, b, b->h / 4, rows);
    d2 = strip_mean_diff(a, a->h / 2 - rows / 2, b, b->h / 2 - rows / 2, rows);
    d3 = strip_mean_diff(a, (3 * a->h) / 4 - rows, b, (3 * b->h) / 4 - rows, rows);
    return (d1 + d2 + d3) / 3.0;
}

static double seam_overlap_diff(const Frame *prev, const Frame *cur, int d) {
    int overlap = prev->h - d;
    int check;

    if (overlap < MIN_MATCH_ROWS) {
        return 1e18;
    }
    check = overlap;
    if (check > 48) {
        check = 48;
    }
    return strip_mean_diff(prev, prev->h - check, cur, 0, check);
}

/* Консервативный выбор d: при сомнении лучше дубль, чем пропуск секции. */
static int finalize_shift(
    const Frame *prev,
    const Frame *cur,
    int found_d,
    int target_d,
    double conf,
    double *seam_out
) {
    int cap;
    int floor_d;
    int d;
    int best_d;
    double best_seam;
    double seam;

    if (!seam_out || target_d < 1) {
        return target_d;
    }

    if (found_d < 1) {
        found_d = target_d;
    }
    if (found_d > cur->h) {
        found_d = cur->h;
    }

    cap = target_d + (int)(target_d * MAX_SHIFT_OVER_FRAC);
    if (cap > found_d) {
        cap = found_d;
    }
    floor_d = target_d - target_d / 12;
    if (floor_d < 8) {
        floor_d = 8;
    }

    best_d = target_d;
    best_seam = seam_overlap_diff(prev, cur, target_d);
    if (best_seam <= SEAM_GOOD) {
        *seam_out = best_seam;
        return target_d;
    }

    for (d = target_d - 1; d >= floor_d; d--) {
        seam = seam_overlap_diff(prev, cur, d);
        if (seam <= SEAM_GOOD) {
            *seam_out = seam;
            log_msg("  conservative shift %dpx (found=%d target=%d)\n", d, found_d, target_d);
            return d;
        }
        if (seam < best_seam) {
            best_seam = seam;
            best_d = d;
        }
    }

    if (conf >= CONF_HIGH) {
        for (d = target_d + 1; d <= cap; d++) {
            seam = seam_overlap_diff(prev, cur, d);
            if (seam <= SEAM_GOOD && seam <= best_seam) {
                best_seam = seam;
                best_d = d;
            }
        }
    }

    if (best_seam > SEAM_MAX_DIFF) {
        best_d = target_d;
        best_seam = seam_overlap_diff(prev, cur, best_d);
    }

    if (found_d > cap) {
        log_msg(
            "  capped shift %dpx -> %dpx (found=%d conf=%.3f)\n",
            found_d,
            best_d,
            found_d,
            conf
        );
    }

    *seam_out = best_seam;
    return best_d;
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

static double canvas_new_strip_diff(const Canvas *canvas, const Frame *cur, int rows) {
    Frame tail;
    int check_rows;

    if (!canvas || !cur || rows < 1 || canvas->h < rows || canvas->w != cur->w) {
        return 1e18;
    }

    check_rows = rows;
    if (check_rows > 64) check_rows = 64;

    memset(&tail, 0, sizeof(tail));
    tail.w = canvas->w;
    tail.h = check_rows;
    tail.px = canvas->px + (size_t)(canvas->h - check_rows) * (size_t)canvas->w * 3u;

    return strip_mean_diff(&tail, 0, cur, cur->h - check_rows, check_rows);
}

static int canvas_append(Canvas *c, const unsigned char *src, int w, int rows, int src_row_start) {
    if (c->w == 0) c->w = w;
    if (w != c->w) return 0;
    if (c->h + rows > MAX_CANVAS_HEIGHT) {
        log_msg("Canvas height limit reached (%d + %d > %d)\n", c->h, rows, MAX_CANVAS_HEIGHT);
        return 0;
    }
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

    int memory_limit_hit = 0;
    RECT r;
    if (!select_region(&r)) {
        MessageBoxA(NULL, "Выделение отменено или область слишком мала.", "vrmshot", MB_OK);
        return 0;
    }

    int x = r.left, y = r.top;
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    log_open();

    /* дать оверлею исчезнуть и пользователю отвести курсор */
    Sleep(400);

    int cx = x + w/2, cy = y + h/2;
    focus_point_once(cx, cy);
    log_msg("Region: %d,%d %dx%d target_scroll~%dpx (focus click once)\n", x, y, w, h, (int)(h * (1.0 - OVERLAP_RATIO)));

    /* Адаптивный шаг прокрутки.
     * ВАЖНО (доказано тестами): прокрутка должна оставлять БОЛЬШОЕ гарантированное
     * перекрытие, иначе на повторяющихся структурах (таблицы, списки с одинаковой
     * высотой строк) шаблон склейки цепляется за соседнюю похожую строку и
     * "проглатывает" данные. Поэтому шаг = (1 - OVERLAP_RATIO) высоты, но НЕ больше
     * безопасного максимума ~0.52*h.
     * Одна "ступенька" колеса = неизвестно сколько пикселей (зависит от
     * приложения), поэтому стартуем с оценки и калибруем по факту. */
    int safe_max_scroll = (int)(h * 0.46);
    int target_scroll = (int)(h * (1.0 - OVERLAP_RATIO));
    if (target_scroll > safe_max_scroll) target_scroll = safe_max_scroll;
    if (target_scroll < 20) target_scroll = 20;

    int px_per_notch = 100;          /* стартовая оценка пикселей на ступеньку */
    int notches = target_scroll / px_per_notch;
    if (notches < 1) notches = 1;

    int target_d = target_scroll;    /* ожидаемый сдвиг для подсказки матчингу */

    Canvas canvas; memset(&canvas, 0, sizeof(canvas));

    Frame prev; memset(&prev, 0, sizeof(prev));
    if (!capture_region_stable(x, y, w, h, &prev)) {
        MessageBoxA(NULL, "Не удалось захватить экран.", "vrmshot", MB_OK);
        log_close();
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
        double conf;
        double quick_diff;
        double bottom_diff;
        double tail_diff;
        double seam_diff = 1e18;
        int d;
        int trust;
        int check_rows;
        int no_progress;
        int hit_bottom;
        int new_rows;
        int seam_ok;

        if (!capture_region_stable(x, y, w, h, &cur)) {
            break;
        }

        quick_diff = frame_quick_diff(&prev, &cur);
        if (quick_diff < SAME_FRAME_DIFF) {
            log_msg(
                "frame=%d no movement quick_diff=%.2f end_repeat=%d\n",
                frames,
                quick_diff,
                end_repeat + 1
            );
            end_repeat++;
            frame_free(&cur);
            if (end_repeat >= END_REPEAT_LIMIT) {
                log_msg("End of page: frame unchanged after scroll.\n");
                break;
            }
            notches += 1;
            continue;
        }

        {
            int found_d;
            int cap_d;

            found_d = find_shift(&prev, &cur, target_d, &conf);
            trust = (conf >= CONF_MIN && found_d >= 1);
            d = finalize_shift(&prev, &cur, found_d, target_d, conf, &seam_diff);
            seam_ok = (seam_diff <= SEAM_MAX_DIFF);

            cap_d = target_d + (int)(target_d * MAX_SHIFT_OVER_FRAC);
            if (found_d > cap_d) {
                if (notches > 1) {
                    notches--;
                }
                px_per_notch = (px_per_notch * 11) / 10;
                if (px_per_notch < 1) {
                    px_per_notch = 1;
                }
            }

            if (!seam_ok) {
                log_msg(
                    "frame=%d bad seam diff=%.2f for d=%d, fallback to target_d=%d\n",
                    frames,
                    seam_diff,
                    d,
                    target_d
                );
                g_bad_seam_streak++;
                d = target_d;
                seam_diff = seam_overlap_diff(&prev, &cur, d);
                seam_ok = (seam_diff <= SEAM_MAX_DIFF);
            } else {
                g_bad_seam_streak = 0;
            }
        }

        if (d > h) d = h;
        if (d < 1) d = 1;

        check_rows = d;
        if (check_rows > 48) check_rows = 48;
        if (check_rows < 8) check_rows = 8;

        bottom_diff = strip_mean_diff(&prev, prev.h - check_rows, &cur, cur.h - check_rows, check_rows);
        tail_diff = canvas_new_strip_diff(&canvas, &cur, d);
        no_progress = (bottom_diff < NO_PROGRESS_DIFF) && (tail_diff < NO_PROGRESS_DIFF);

        log_msg(
            "frame=%d shift=%dpx conf=%.3f seam=%.2f bottom=%.2f tail=%.2f window=%.2f\n",
            frames,
            d,
            conf,
            seam_diff,
            bottom_diff,
            tail_diff,
            current_shift_window_frac()
        );

        hit_bottom = (trust && d <= 4);
        if (no_progress || hit_bottom) {
            if (hit_bottom && !no_progress && d >= 1 && d <= h) {
                if (!canvas_append(&canvas, cur.px, cur.w, d, h - d)) {
                    memory_limit_hit = 1;
                    frame_free(&cur);
                    break;
                }
            }
            end_repeat++;
            frame_free(&cur);
            if (end_repeat >= END_REPEAT_LIMIT) {
                log_msg("End of page: no further progress.\n");
                break;
            }
            notches += 1;
            continue;
        }
        end_repeat = 0;

        new_rows = d;

        if (d > 4 && notches > 0 && trust && seam_ok) {
            int measured = d / notches;
            if (measured > 2) {
                px_per_notch = (px_per_notch * 2 + measured) / 3;
                if (px_per_notch < 1) px_per_notch = 1;
                {
                    int nn = target_scroll / px_per_notch;
                    if (nn < 1) nn = 1;
                    notches = nn;
                }
            }
            target_d = (target_d * 3 + d) / 4;
            if (target_d > target_scroll) target_d = target_scroll;
            if (target_d < 8) target_d = 8;
        }

        if (!canvas_append(&canvas, cur.px, cur.w, new_rows, h - new_rows)) {
            memory_limit_hit = 1;
            frame_free(&cur);
            break;
        }

        frame_free(&prev);
        prev = cur;
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
        if (memory_limit_hit) {
            strcat(msg, "\n\nПредупреждение: достигнут лимит высоты, сохранена часть страницы.");
        }
        log_msg("Done: %s (%d x %d px, frames=%d)\n", fname, canvas.w, canvas.h, frames);
    } else {
        sprintf(msg, "Ошибка записи PNG.");
        log_msg("PNG write failed.\n");
    }
    MessageBoxA(NULL, msg, "vrmshot", MB_OK);

    free(canvas.px);
    log_close();
    return ok ? 0 : 1;
}
