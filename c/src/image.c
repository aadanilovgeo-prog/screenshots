#include "scroll_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sc_image_size_ok(int width, int height) {
    size_t bytes;

    if (width <= 0 || height <= 0 || height > SC_MAX_OUTPUT_HEIGHT) {
        return 0;
    }

    bytes = (size_t)width * (size_t)height * 3u;
    return bytes <= SC_MAX_IMAGE_BYTES;
}

ScImage *sc_image_create(int width, int height) {
    ScImage *img;
    size_t bytes;

    if (!sc_image_size_ok(width, height)) {
        return NULL;
    }

    img = (ScImage *)calloc(1, sizeof(ScImage));
    if (!img) {
        return NULL;
    }

    bytes = (size_t)width * (size_t)height * 3u;
    img->rgb = (unsigned char *)malloc(bytes);
    if (!img->rgb) {
        free(img);
        return NULL;
    }

    img->width = width;
    img->height = height;
    return img;
}

void sc_image_free(ScImage *img) {
    if (!img) {
        return;
    }
    free(img->rgb);
    free(img);
}

ScImage *sc_image_copy(const ScImage *src) {
    ScImage *dst;
    size_t bytes;

    if (!src || !src->rgb) {
        return NULL;
    }

    dst = sc_image_create(src->width, src->height);
    if (!dst) {
        return NULL;
    }

    bytes = (size_t)src->width * (size_t)src->height * 3u;
    memcpy(dst->rgb, src->rgb, bytes);
    return dst;
}

double sc_image_diff_ratio(const ScImage *a, const ScImage *b) {
    size_t i;
    size_t pixels;
    long long sum = 0;

    if (!a || !b || !a->rgb || !b->rgb) {
        return 1.0;
    }
    if (a->width != b->width || a->height != b->height) {
        return 1.0;
    }

    pixels = (size_t)a->width * (size_t)a->height * 3u;
    for (i = 0; i < pixels; i++) {
        sum += abs((int)a->rgb[i] - (int)b->rgb[i]);
    }

    return (sum / (double)pixels) / 255.0;
}

void sc_frame_list_init(ScFrameList *list) {
    if (!list) {
        return;
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int sc_frame_list_push(ScFrameList *list, ScImage *img) {
    ScImage **next;

    if (!list || !img) {
        return 0;
    }

    if (list->count >= list->capacity) {
        int new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
        next = (ScImage **)realloc(list->items, (size_t)new_cap * sizeof(ScImage *));
        if (!next) {
            return 0;
        }
        list->items = next;
        list->capacity = new_cap;
    }

    list->items[list->count++] = img;
    return 1;
}

void sc_frame_list_clear(ScFrameList *list) {
    int i;

    if (!list) {
        return;
    }

    for (i = 0; i < list->count; i++) {
        sc_image_free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

ScImage *sc_image_append_crop(const ScImage *result, const ScImage *frame, int top_crop) {
    ScImage *out;
    int overlap;
    int keep;
    int new_h;
    int y;
    int row_bytes;

    if (!result || !frame || !result->rgb || !frame->rgb) {
        return NULL;
    }
    if (result->width != frame->width) {
        return NULL;
    }

    overlap = top_crop;
    if (overlap < 0) {
        overlap = 0;
    }
    if (overlap >= frame->height) {
        overlap = frame->height - 1;
    }

    keep = result->height - overlap;
    if (keep < 0) {
        keep = 0;
    }

    new_h = keep + frame->height;
    out = sc_image_create(result->width, new_h);
    if (!out) {
        return NULL;
    }

    row_bytes = result->width * 3;
    if (keep > 0) {
        memcpy(out->rgb, result->rgb, (size_t)keep * (size_t)row_bytes);
    }

    memcpy(
        out->rgb + (size_t)keep * (size_t)row_bytes,
        frame->rgb,
        (size_t)frame->height * (size_t)row_bytes
    );

    for (y = 0; y < overlap; y++) {
        int src_y = result->height - overlap + y;
        if (src_y < 0) {
            src_y = 0;
        }
        memcpy(
            out->rgb + (size_t)(keep + y) * (size_t)row_bytes,
            result->rgb + (size_t)src_y * (size_t)row_bytes,
            (size_t)row_bytes
        );
    }

    return out;
}
