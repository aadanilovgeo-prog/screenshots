#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "scroll_capture.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

void sc_sleep_ms(int milliseconds) {
    Sleep((DWORD)milliseconds);
}

void sc_get_cursor_pos(int *x, int *y) {
    POINT pt;
    if (GetCursorPos(&pt)) {
        *x = (int)pt.x;
        *y = (int)pt.y;
    }
}

int sc_mkdir_p(const char *path) {
    char buf[512];
    size_t len;
    size_t i;

    if (!path || !path[0]) {
        return 0;
    }

    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    len = strlen(buf);
    for (i = 1; i < len; i++) {
        if (buf[i] == '\\' || buf[i] == '/') {
            char saved = buf[i];
            buf[i] = '\0';
            CreateDirectoryA(buf, NULL);
            buf[i] = saved;
        }
    }
    return CreateDirectoryA(buf, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

int sc_focus_region(const ScRegion *region) {
    int x = region->left + region->width / 2;
    int y = region->top + region->height / 2;

    SetCursorPos(x, y);
    sc_sleep_ms(50);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    sc_sleep_ms(80);
    return 1;
}

static void wheel_at(int x, int y, int notches_down) {
    DWORD delta;
    SetCursorPos(x, y);
    delta = (DWORD)(-(int)(SC_WHEEL_DELTA * notches_down));
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, delta, 0);
}

void sc_scroll_one_notch(const ScRegion *region, const ScScrollSettings *scroll) {
    int x = region->left + region->width / 2;
    int y = region->top + region->height / 2;

    if (scroll->focus_each_step) {
        sc_focus_region(region);
    }

    wheel_at(x, y, 1);
}

int sc_capture_region(const ScRegion *region, ScImage *out) {
    HDC screen_dc;
    HDC mem_dc;
    HBITMAP bitmap;
    HBITMAP old_bitmap;
    BITMAPINFO bmi;
    unsigned char *bgr = NULL;
    int x;
    int y;
    int row_bytes;
    size_t buf_size;

    if (!region || !out || !out->rgb) {
        return 0;
    }

    screen_dc = GetDC(NULL);
    if (!screen_dc) {
        return 0;
    }

    mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(NULL, screen_dc);
        return 0;
    }

    bitmap = CreateCompatibleBitmap(screen_dc, region->width, region->height);
    if (!bitmap) {
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return 0;
    }

    old_bitmap = (HBITMAP)SelectObject(mem_dc, bitmap);
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
    if (!bgr) {
        SelectObject(mem_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        return 0;
    }

    if (!GetDIBits(mem_dc, bitmap, 0, (UINT)region->height, bgr, &bmi, DIB_RGB_COLORS)) {
        free(bgr);
        SelectObject(mem_dc, old_bitmap);
        DeleteObject(bitmap);
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
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
    return 1;
}

int sc_pick_region_interactive(ScRegion *region) {
    int x1, y1, x2, y2;

    printf("Interactive capture region selection.\n");
    printf("Move cursor to TOP-LEFT corner of the article and press Enter...\n");
    getchar();
    sc_get_cursor_pos(&x1, &y1);
    printf("  Top-left corner: (%d, %d)\n", x1, y1);

    printf("Move cursor to BOTTOM-RIGHT corner of the article and press Enter...\n");
    getchar();
    sc_get_cursor_pos(&x2, &y2);
    printf("  Bottom-right corner: (%d, %d)\n", x2, y2);

    region->left = x1 < x2 ? x1 : x2;
    region->top = y1 < y2 ? y1 : y2;
    region->width = abs(x2 - x1);
    region->height = abs(y2 - y1);

    if (region->width < 50 || region->height < 50) {
        fprintf(stderr, "Capture region is too small.\n");
        return 0;
    }
    return 1;
}

void sc_countdown(int seconds, const char *message) {
    int remaining;
    printf("%s\n", message);
    for (remaining = seconds; remaining > 0; remaining--) {
        printf("  %d...\n", remaining);
        fflush(stdout);
        sc_sleep_ms(1000);
    }
    printf("  Start!\n");
}

int sc_make_default_output_path(char *buf, size_t buflen) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return snprintf(
        buf,
        buflen,
        "long_screenshot_%04d-%02d-%02d_%02d-%02d-%02d.png",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond
    );
}

#else

#include "scroll_capture.h"
#include "version.h"

#include <stdio.h>

void sc_sleep_ms(int milliseconds) {
    (void)milliseconds;
}

void sc_get_cursor_pos(int *x, int *y) {
    *x = 0;
    *y = 0;
}

int sc_mkdir_p(const char *path) {
    (void)path;
    return 0;
}

int sc_focus_region(const ScRegion *region) {
    (void)region;
    return 0;
}

void sc_scroll_one_notch(const ScRegion *region, const ScScrollSettings *scroll) {
    (void)region;
    (void)scroll;
}

int sc_capture_region(const ScRegion *region, ScImage *out) {
    (void)region;
    (void)out;
    return 0;
}

int sc_pick_region_interactive(ScRegion *region) {
    (void)region;
    fprintf(stderr, "Interactive mode is only available on Windows.\n");
    return 0;
}

void sc_countdown(int seconds, const char *message) {
    (void)seconds;
    (void)message;
}

int sc_make_default_output_path(char *buf, size_t buflen) {
    return snprintf(buf, buflen, "long_screenshot.png");
}

#endif
