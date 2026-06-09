#include "updated_screenshoter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    float *gray;
    float *profile;
    float *grad_profile;
    int width;
    int height;
} MatchBuf;

static int compare_int(const void *a, const void *b) {
    return (*(const int *)a) - (*(const int *)b);
}

static float ncc(const float *a, const float *b, int len) {
    float ma = 0, mb = 0, num = 0, da, db, da2 = 0, db2 = 0;
    int i;
    if (len < 2) {
        return 0.0f;
    }
    for (i = 0; i < len; i++) {
        ma += a[i];
        mb += b[i];
    }
    ma /= (float)len;
    mb /= (float)len;
    for (i = 0; i < len; i++) {
        da = a[i] - ma;
        db = b[i] - mb;
        num += da * db;
        da2 += da * da;
        db2 += db * db;
    }
    return num / (sqrtf(da2 * db2) + 1e-5f);
}

static void match_buf_free(MatchBuf *b) {
    free(b->gray);
    free(b->profile);
    free(b->grad_profile);
    memset(b, 0, sizeof(*b));
}

static int match_buf_build(const UsImage *img, MatchBuf *b, int skip_top_rows) {
    int x, y, margin_x, cw;
    float *full = NULL;

    memset(b, 0, sizeof(*b));
    b->width = img->width;
    b->height = img->height;

    margin_x = img->width / 8;
    if (margin_x < 8) {
        margin_x = 8;
    }
    cw = img->width - margin_x * 2;
    if (cw < 8) {
        return 0;
    }

    full = (float *)malloc((size_t)img->width * (size_t)img->height * sizeof(float));
    b->gray = (float *)malloc((size_t)cw * (size_t)img->height * sizeof(float));
    b->profile = (float *)malloc((size_t)img->height * sizeof(float));
    b->grad_profile = (float *)malloc((size_t)img->height * sizeof(float));
    if (!full || !b->gray || !b->profile || !b->grad_profile) {
        free(full);
        match_buf_free(b);
        return 0;
    }

    for (y = 0; y < img->height; y++) {
        float row_sum = 0, grad_sum = 0, prev = 0;
        int has_prev = 0;
        for (x = 0; x < cw; x++) {
            int sx = x + margin_x;
            int idx = (y * img->width + sx) * 3;
            float lum = 0.299f * img->rgb[idx] + 0.587f * img->rgb[idx + 1] + 0.114f * img->rgb[idx + 2];
            b->gray[y * cw + x] = lum;
            full[y * img->width + sx] = lum;
            row_sum += lum;
            if (has_prev) {
                grad_sum += fabsf(lum - prev);
            }
            prev = lum;
            has_prev = 1;
        }
        b->profile[y] = row_sum / (float)cw;
        b->grad_profile[y] = grad_sum / (float)cw;
        (void)full;
    }

    free(full);
    b->width = cw;
    (void)skip_top_rows;
    return 1;
}

static double match_cost(const MatchBuf *above, const MatchBuf *below, int overlap, int skip_top) {
    int y, x;
    double ssd = 0;
    int pixels = 0;
    float ni, ng;
    int start_row = skip_top;

    if (overlap <= start_row + 4) {
        return 1e9;
    }

    for (y = start_row; y < overlap; y++) {
        const float *ra = above->gray + (above->height - overlap + y) * above->width;
        const float *rb = below->gray + y * below->width;
        for (x = 0; x < above->width; x += 2) {
            ssd += fabs((double)ra[x] - (double)rb[x]);
            pixels++;
        }
    }
    ssd /= (double)pixels;

    ni = ncc(above->profile + above->height - overlap, below->profile, overlap);
    ng = ncc(above->grad_profile + above->height - overlap, below->grad_profile, overlap);
    return ssd / 255.0 - 0.55 * (double)ni - 0.35 * (double)ng;
}

static double cost_to_confidence(double cost) {
    double c = 1.0 - cost;
    if (c < 0.0) {
        c = 0.0;
    }
    if (c > 1.0) {
        c = 1.0;
    }
    return c;
}

UsOverlapResult us_find_overlap(const UsImage *previous, const UsImage *current, const UsPageMetrics *metrics) {
    MatchBuf ma, mb;
    UsOverlapResult result;
    int min_o, max_o, best_o, o;
    double best_cost = 1e300, second_cost = 1e300;
    int search_px;
    int skip_top;
    int h;

    result.overlap = metrics ? metrics->expected_overlap : 100;
    result.confidence = 0.0;
    result.used_fallback = 1;

    if (!previous || !current || !metrics) {
        return result;
    }

    h = previous->height < current->height ? previous->height : current->height;
    search_px = (int)(h * US_OVERLAP_SEARCH_FRAC);
    skip_top = (int)(h * US_HEADER_EXCLUDE_FRAC);
    if (skip_top < 8) {
        skip_top = 8;
    }

    min_o = metrics->expected_overlap - (int)(h * 0.08);
    max_o = metrics->expected_overlap + (int)(h * 0.08);
    if (min_o < search_px / 2) {
        min_o = search_px / 2;
    }
    if (max_o > search_px) {
        max_o = search_px;
    }
    if (max_o > h - 20) {
        max_o = h - 20;
    }
    if (min_o >= max_o) {
        min_o = metrics->expected_overlap - 20;
        max_o = metrics->expected_overlap + 20;
        if (min_o < 30) {
            min_o = 30;
        }
        if (max_o > h - 10) {
            max_o = h - 10;
        }
    }

    if (!match_buf_build(previous, &ma, skip_top) || !match_buf_build(current, &mb, 0)) {
        return result;
    }

    best_o = min_o;
    for (o = min_o; o <= max_o; o++) {
        double cost = match_cost(&ma, &mb, o, skip_top);
        if (cost < best_cost) {
            second_cost = best_cost;
            best_cost = cost;
            best_o = o;
        } else if (cost < second_cost) {
            second_cost = cost;
        }
    }

    for (o = best_o - 6; o <= best_o + 6; o++) {
        double cost;
        if (o < min_o || o > max_o) {
            continue;
        }
        cost = match_cost(&ma, &mb, o, skip_top);
        if (cost < best_cost) {
            second_cost = best_cost;
            best_cost = cost;
            best_o = o;
        }
    }

    result.confidence = cost_to_confidence(best_cost);
    if (second_cost < 1e200) {
        double sep = second_cost - best_cost;
        if (sep < 0.02) {
            result.confidence *= 0.75;
        }
    }

    if (result.confidence >= US_MIN_CONFIDENCE) {
        result.overlap = best_o;
        result.used_fallback = 0;
    } else {
        result.overlap = metrics->expected_overlap;
        result.used_fallback = 1;
        printf("  [fallback] low overlap confidence %.2f, using expected overlap %d px\n",
               result.confidence, result.overlap);
    }

    if (result.overlap > h - 5) {
        result.overlap = h - 5;
    }
    if (result.overlap < 20) {
        result.overlap = 20;
    }

    match_buf_free(&ma);
    match_buf_free(&mb);
    return result;
}

int us_stabilize_overlaps(int *overlaps, int *fallbacks, int count) {
    int *copy, median, i;
    if (!overlaps || count <= 2) {
        return count;
    }

    copy = (int *)malloc((size_t)count * sizeof(int));
    if (!copy) {
        return count;
    }

    memcpy(copy, overlaps, (size_t)count * sizeof(int));
    qsort(copy, (size_t)count, sizeof(int), compare_int);
    median = copy[count / 2];

    for (i = 0; i < count; i++) {
        if (fallbacks && fallbacks[i]) {
            overlaps[i] = median;
        } else if (abs(overlaps[i] - median) > (int)(median * 0.15) + 30) {
            overlaps[i] = median;
        }
    }

    free(copy);
    return count;
}

UsImage *us_stitch_images(const UsFrameList *frames, const int *overlaps, int overlap_count) {
    int i, total_h = 0, y = 0, overlap, row_bytes;
    UsImage *out;
    unsigned char *dst;

    if (!frames || frames->count == 0) {
        return NULL;
    }
    if (frames->count == 1) {
        return us_image_copy(frames->items[0]);
    }
    if (!overlaps || overlap_count != frames->count - 1) {
        return NULL;
    }

    total_h = frames->items[0]->height;
    for (i = 1; i < frames->count; i++) {
        overlap = overlaps[i - 1];
        if (overlap < 1) {
            overlap = 1;
        }
        if (overlap > frames->items[i]->height - 1) {
            overlap = frames->items[i]->height - 1;
        }
        total_h += frames->items[i]->height - overlap;
    }

    out = us_image_create(frames->items[0]->width, total_h);
    if (!out) {
        return NULL;
    }

    dst = out->rgb;
    row_bytes = frames->items[0]->width * 3;
    memcpy(dst, frames->items[0]->rgb, (size_t)row_bytes * (size_t)frames->items[0]->height);
    y = frames->items[0]->height;

    for (i = 1; i < frames->count; i++) {
        int src_y, src_h;
        overlap = overlaps[i - 1];
        if (overlap < 1) {
            overlap = 1;
        }
        if (overlap > frames->items[i]->height - 1) {
            overlap = frames->items[i]->height - 1;
        }
        src_h = frames->items[i]->height - overlap;
        for (src_y = 0; src_y < src_h; src_y++) {
            memcpy(
                dst + (size_t)(y + src_y) * (size_t)row_bytes,
                frames->items[i]->rgb + (size_t)(overlap + src_y) * (size_t)row_bytes,
                (size_t)row_bytes
            );
        }
        y += src_h;
    }

    return out;
}

UsImage *us_finalize_long_screenshot(UsImage *stitched, int content_height) {
    if (!stitched || content_height <= 0 || content_height >= stitched->height) {
        return stitched;
    }
    {
        UsImage *trimmed = us_image_create(stitched->width, content_height);
        if (!trimmed) {
            return stitched;
        }
        memcpy(
            trimmed->rgb,
            stitched->rgb,
            (size_t)stitched->width * (size_t)content_height * 3u
        );
        us_image_free(stitched);
        return trimmed;
    }
}
