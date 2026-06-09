#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include "updated_screenshoter.h"

#include <stdio.h>
#include <string.h>

void us_sleep_ms(int ms) {
    Sleep((DWORD)ms);
}

void us_get_cursor_pos(int *x, int *y) {
    POINT pt;
    if (GetCursorPos(&pt)) {
        *x = (int)pt.x;
        *y = (int)pt.y;
    }
}

int us_focus_region(const UsRegion *region) {
    int cx = region->left + region->width / 2;
    int cy = region->top + region->height / 2;
    SetCursorPos(cx, cy);
    us_sleep_ms(60);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    us_sleep_ms(100);
    return 1;
}

static void wheel_down(int x, int y, int notches) {
    SetCursorPos(x, y);
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(-(int)(US_WHEEL_DELTA * notches)), 0);
}

int us_scroll_to_next_position(const UsRegion *region, const UsPageMetrics *metrics) {
    int cx = region->left + region->width / 2;
    int cy = region->top + region->height / 2;
    int target_px = metrics->scroll_step;
    int notch_px = (int)(region->height * 0.06);
    int notches;
    int i;

    if (notch_px < 20) {
        notch_px = 20;
    }
    notches = target_px / notch_px;
    if (notches < 2) {
        notches = 2;
    }
    if (notches > 24) {
        notches = 24;
    }

    SetCursorPos(cx, cy);
    for (i = 0; i < notches; i++) {
        wheel_down(cx, cy, 1);
        us_sleep_ms(35);
    }
    return 1;
}

int us_capture_viewport(const UsRegion *region, UsImage *out) {
    HDC screen_dc, mem_dc;
    HBITMAP bmp, old;
    BITMAPINFO bmi;
    unsigned char *bgr = NULL;
    int row_bytes, x, y;
    size_t buf_size;

    screen_dc = GetDC(NULL);
    mem_dc = CreateCompatibleDC(screen_dc);
    bmp = CreateCompatibleBitmap(screen_dc, region->width, region->height);
    old = (HBITMAP)SelectObject(mem_dc, bmp);
    BitBlt(mem_dc, 0, 0, region->width, region->height, screen_dc, region->left, region->top, SRCCOPY);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = region->width;
    bmi.bmiHeader.biHeight = -region->height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    row_bytes = ((region->width * 3 + 3) / 4) * 4;
    buf_size = (size_t)row_bytes * (size_t)region->height;
    bgr = (unsigned char *)malloc(buf_size);
    if (!bgr || !GetDIBits(mem_dc, bmp, 0, (UINT)region->height, bgr, &bmi, DIB_RGB_COLORS)) {
        free(bgr);
        SelectObject(mem_dc, old);
        DeleteObject(bmp);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return 0;
    }

    for (y = 0; y < region->height; y++) {
        const unsigned char *src = bgr + (size_t)y * (size_t)row_bytes;
        unsigned char *dst = out->rgb + (size_t)y * (size_t)region->width * 3u;
        for (x = 0; x < region->width; x++) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }

    free(bgr);
    SelectObject(mem_dc, old);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
    return 1;
}

int us_wait_for_page_stable(const UsRegion *region, int stable_ms, int max_wait_ms) {
    UsImage *a = NULL;
    UsImage *b = NULL;
    int elapsed = 0;
    int stable_count = 0;
    double diff;

    a = us_image_create(region->width, region->height);
    b = us_image_create(region->width, region->height);
    if (!a || !b) {
        us_image_free(a);
        us_image_free(b);
        us_sleep_ms(stable_ms);
        return 0;
    }

    us_capture_viewport(region, a);
    while (elapsed < max_wait_ms) {
        us_sleep_ms(80);
        elapsed += 80;
        us_capture_viewport(region, b);
        diff = us_image_diff_ratio(a, b);
        if (diff < 0.003) {
            stable_count++;
            if (stable_count >= 2) {
                break;
            }
        } else {
            stable_count = 0;
        }
        {
            unsigned char *tmp = a->rgb;
            a->rgb = b->rgb;
            b->rgb = tmp;
        }
    }

    us_image_free(a);
    us_image_free(b);
    us_sleep_ms(stable_ms);
    return 1;
}

int us_pick_region_interactive(UsRegion *region) {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    printf("Interactive region selection.\n");
    printf("Move cursor to TOP-LEFT of article, press Enter...\n");
    getchar();
    us_get_cursor_pos(&x1, &y1);
    printf("  Top-left: (%d, %d)\n", x1, y1);
    printf("Move cursor to BOTTOM-RIGHT of article, press Enter...\n");
    getchar();
    us_get_cursor_pos(&x2, &y2);
    printf("  Bottom-right: (%d, %d)\n", x2, y2);

    region->left = x1 < x2 ? x1 : x2;
    region->top = y1 < y2 ? y1 : y2;
    region->width = abs(x2 - x1);
    region->height = abs(y2 - y1);
    if (region->width < 50 || region->height < 50) {
        fprintf(stderr, "Region too small.\n");
        return 0;
    }
    return 1;
}

void us_countdown(int seconds, const char *message) {
    int r;
    printf("%s\n", message);
    for (r = seconds; r > 0; r--) {
        printf("  %d...\n", r);
        fflush(stdout);
        us_sleep_ms(1000);
    }
    printf("  Start!\n");
}

int us_make_output_path(char *buf, size_t buflen, int to_downloads) {
    SYSTEMTIME st;
    char base[512];
    char fname[128];

    GetLocalTime(&st);
    snprintf(
        fname,
        sizeof(fname),
        "long_screenshot_%04d-%02d-%02d_%02d-%02d-%02d.png",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond
    );

    if (to_downloads && SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, base))) {
        snprintf(buf, buflen, "%s\\Downloads\\%s", base, fname);
        return 1;
    }

    if (GetModuleFileNameA(NULL, base, (DWORD)sizeof(base)) > 0) {
        char *slash = strrchr(base, '\\');
        if (slash) {
            *(slash + 1) = '\0';
        }
        snprintf(buf, buflen, "%s%s", base, fname);
        return 1;
    }

    snprintf(buf, buflen, "%s", fname);
    return 1;
}

#else

#include "updated_screenshoter.h"
#include <stdio.h>

void us_sleep_ms(int ms) { (void)ms; }
void us_get_cursor_pos(int *x, int *y) { *x = 0; *y = 0; }
int us_focus_region(const UsRegion *r) { (void)r; return 0; }
int us_scroll_to_next_position(const UsRegion *r, const UsPageMetrics *m) { (void)r; (void)m; return 0; }
int us_capture_viewport(const UsRegion *r, UsImage *o) { (void)r; (void)o; return 0; }
int us_wait_for_page_stable(const UsRegion *r, int a, int b) { (void)r; (void)a; (void)b; return 0; }
int us_pick_region_interactive(UsRegion *r) { (void)r; fprintf(stderr, "Windows only.\n"); return 0; }
void us_countdown(int s, const char *m) { (void)s; (void)m; }
int us_make_output_path(char *b, size_t n, int d) { return snprintf(b, n, "long_screenshot.png"); }

#endif
