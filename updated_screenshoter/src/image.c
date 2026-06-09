#include "updated_screenshoter.h"

#include <stdlib.h>
#include <string.h>

UsImage *us_image_create(int width, int height) {
    UsImage *img;
    size_t bytes;

    if (width <= 0 || height <= 0) {
        return NULL;
    }

    img = (UsImage *)calloc(1, sizeof(UsImage));
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

void us_image_free(UsImage *img) {
    if (!img) {
        return;
    }
    free(img->rgb);
    free(img);
}

UsImage *us_image_copy(const UsImage *src) {
    UsImage *dst;
    size_t bytes;

    if (!src || !src->rgb) {
        return NULL;
    }

    dst = us_image_create(src->width, src->height);
    if (!dst) {
        return NULL;
    }

    bytes = (size_t)src->width * (size_t)src->height * 3u;
    memcpy(dst->rgb, src->rgb, bytes);
    return dst;
}

double us_image_diff_ratio(const UsImage *a, const UsImage *b) {
    size_t i;
    size_t pixels;
    long long sum = 0;

    if (!a || !b || !a->rgb || !b->rgb || a->width != b->width || a->height != b->height) {
        return 1.0;
    }

    pixels = (size_t)a->width * (size_t)a->height * 3u;
    for (i = 0; i < pixels; i++) {
        sum += abs((int)a->rgb[i] - (int)b->rgb[i]);
    }
    return (sum / (double)pixels) / 255.0;
}

UsImage *us_crop_top(const UsImage *src, int crop_rows) {
    UsImage *dst;
    int new_h;
    int row_bytes;

    if (!src || crop_rows <= 0 || crop_rows >= src->height) {
        return us_image_copy(src);
    }

    new_h = src->height - crop_rows;
    dst = us_image_create(src->width, new_h);
    if (!dst) {
        return NULL;
    }

    row_bytes = src->width * 3;
    memcpy(dst->rgb, src->rgb + (size_t)crop_rows * (size_t)row_bytes, (size_t)new_h * (size_t)row_bytes);
    return dst;
}

void us_frame_list_init(UsFrameList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int us_frame_list_push(UsFrameList *list, UsImage *img) {
    if (!list || !img) {
        return 0;
    }
    if (list->count >= list->capacity) {
        int cap = list->capacity == 0 ? 8 : list->capacity * 2;
        UsImage **n = (UsImage **)realloc(list->items, (size_t)cap * sizeof(UsImage *));
        if (!n) {
            return 0;
        }
        list->items = n;
        list->capacity = cap;
    }
    list->items[list->count++] = img;
    return 1;
}

void us_frame_list_clear(UsFrameList *list) {
    int i;
    if (!list) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        us_image_free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}
