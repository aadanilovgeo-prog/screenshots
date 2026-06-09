#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/scroll_capture.h"

static void fill_page(unsigned char *rgb, int w, int page_h) {
    int y, x;
    for (y = 0; y < page_h; y++) {
        for (x = 0; x < w; x++) {
            int v = (y * 17 + x * 3) % 200 + 30;
            rgb[(y * w + x) * 3 + 0] = (unsigned char)v;
            rgb[(y * w + x) * 3 + 1] = (unsigned char)((v + 40) % 255);
            rgb[(y * w + x) * 3 + 2] = (unsigned char)((v + 80) % 255);
        }
    }
}

int main(void) {
    const int w = 800, h = 600, page_h = 5000;
    const int scroll = 8 * SC_PX_PER_WHEEL_NOTCH;
    unsigned char *page = (unsigned char *)malloc((size_t)w * page_h * 3);
    ScFrameList frames;
    int overlaps[64];
    int overlap_count = 0;
    int y = 0;
    int has_preferred = 0;
    int preferred = 0;
    ScImage *result;

    assert(page);
    fill_page(page, w, page_h);
    sc_frame_list_init(&frames);

    while (y < page_h - h) {
        ScImage *img = sc_image_create(w, h);
        assert(img);
        memcpy(img->rgb, page + (size_t)y * w * 3, (size_t)w * h * 3);
        assert(sc_frame_list_push(&frames, img));
        y += scroll;
    }
    {
        ScImage *img = sc_image_create(w, h);
        memcpy(img->rgb, page + (size_t)(page_h - h) * w * 3, (size_t)w * h * 3);
        sc_frame_list_push(&frames, img);
    }

    for (int i = 0; i < frames.count - 1; i++) {
        int min_o, max_o;
        sc_overlap_search_bounds(h, 8, 0, preferred, has_preferred, &min_o, &max_o);
        ScOverlapMatch m = sc_find_vertical_overlap(
            frames.items[i], frames.items[i + 1], min_o, max_o, preferred, has_preferred);
        overlaps[overlap_count++] = m.overlap;
        preferred = m.overlap;
        has_preferred = 1;
    }

    sc_stabilize_overlaps(overlaps, overlap_count);
    result = sc_stitch_frames(&frames, overlaps, overlap_count);
    assert(result);
    printf("stitch height=%d expected=%d\n", result->height, page_h);
    assert(abs(result->height - page_h) < 120);

    sc_image_free(result);
    sc_frame_list_clear(&frames);
    free(page);
    printf("stitch_test OK\n");
    return 0;
}
