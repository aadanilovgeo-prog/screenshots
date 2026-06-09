#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/scroll_capture.h"

static void fill_page(unsigned char *rgb, int w, int page_h) {
    int y, x;
    for (y = 0; y < page_h; y++) {
        for (x = 0; x < w; x++) {
            int v = (y * 131 + x * 7) % 220 + 20;
            rgb[(y * w + x) * 3 + 0] = (unsigned char)(v);
            rgb[(y * w + x) * 3 + 1] = (unsigned char)((v + 37) % 255);
            rgb[(y * w + x) * 3 + 2] = (unsigned char)((v + 91) % 255);
        }
    }
}

int main(void) {
    const int w = 800, h = 600, page_h = 5000;
    const int scroll = 280;
    unsigned char *page = (unsigned char *)malloc((size_t)w * page_h * 3);
    ScFrameList frames;
    int crops[64];
    int crop_count = 0;
    int y = 0;
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
        ScShiftData shift;
        ScSafeCrop crop;
        assert(sc_detect_vertical_content_shift(frames.items[i], frames.items[i + 1], &shift));
        crop = sc_choose_safe_crop(frames.items[i], frames.items[i + 1], &shift, 1);
        crops[crop_count++] = crop.crop;
        printf(
            "frame %d crop=%d shift=%d conf=%.2f loss=%.3f dup=%.3f\n",
            i + 1,
            crop.crop,
            shift.detected_shift,
            shift.confidence,
            crop.content_loss_risk,
            crop.duplicate_risk
        );
    }

    result = sc_stitch_frames_safe(&frames, crops, crop_count);
    assert(result);
    printf("stitch height=%d expected=%d delta=%d\n", result->height, page_h, abs(result->height - page_h));
    assert(abs(result->height - page_h) < 120);

    sc_image_free(result);
    sc_frame_list_clear(&frames);
    free(page);
    printf("stitch_test OK\n");
    return 0;
}
