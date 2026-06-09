#include "scroll_capture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_HEADER_SKIP_FRAC 0.12
#define SC_SEAM_GOOD 0.045
#define SC_SEAM_BAD 0.075

typedef struct {
    float *gray;
    float *intensity_profile;
    float *gradient_profile;
    int width;
    int height;
} MatchData;

static int compare_int(const void *a, const void *b) {
    int av = *(const int *)a;
    int bv = *(const int *)b;
    return (av > bv) - (av < bv);
}

static float ncc(const float *a, const float *b, int len) {
    float mean_a = 0.0f;
    float mean_b = 0.0f;
    float num = 0.0f;
    float den_a = 0.0f;
    float den_b = 0.0f;
    int i;

    if (len < 2) {
        return 0.0f;
    }

    for (i = 0; i < len; i++) {
        mean_a += a[i];
        mean_b += b[i];
    }
    mean_a /= (float)len;
    mean_b /= (float)len;

    for (i = 0; i < len; i++) {
        float da = a[i] - mean_a;
        float db = b[i] - mean_b;
        num += da * db;
        den_a += da * da;
        den_b += db * db;
    }

    return num / (sqrtf(den_a * den_b) + 1e-6f);
}

static void match_data_free(MatchData *md) {
    if (!md) {
        return;
    }
    free(md->gray);
    free(md->intensity_profile);
    free(md->gradient_profile);
    memset(md, 0, sizeof(*md));
}

static int match_data_prepare(const ScImage *img, MatchData *md) {
    int x;
    int y;
    int margin_x;
    int cropped_w;

    if (!img || !img->rgb || !md) {
        return 0;
    }

    memset(md, 0, sizeof(*md));
    md->width = img->width;
    md->height = img->height;
    margin_x = img->width / 8;
    if (margin_x < 8) {
        margin_x = 8;
    }
    cropped_w = img->width - margin_x * 2;
    if (cropped_w < 8) {
        return 0;
    }

    md->intensity_profile = (float *)malloc((size_t)img->height * sizeof(float));
    md->gradient_profile = (float *)malloc((size_t)img->height * sizeof(float));
    md->gray = (float *)malloc((size_t)cropped_w * (size_t)img->height * sizeof(float));

    if (!md->intensity_profile || !md->gradient_profile || !md->gray) {
        match_data_free(md);
        return 0;
    }

    for (y = 0; y < img->height; y++) {
        float row_sum = 0.0f;
        float grad_sum = 0.0f;
        float prev = 0.0f;
        int has_prev = 0;

        for (x = 0; x < cropped_w; x++) {
            int src_x = x + margin_x;
            int idx = (y * img->width + src_x) * 3;
            float lum = 0.299f * img->rgb[idx] + 0.587f * img->rgb[idx + 1] + 0.114f * img->rgb[idx + 2];
            md->gray[y * cropped_w + x] = lum;
            row_sum += lum;
            if (has_prev) {
                grad_sum += fabsf(lum - prev);
            }
            prev = lum;
            has_prev = 1;
        }

        md->intensity_profile[y] = row_sum / (float)cropped_w;
        md->gradient_profile[y] = grad_sum / (float)cropped_w;
    }

    md->width = cropped_w;
    return 1;
}

static double seam_mean_diff(const ScImage *above, const ScImage *below, int overlap) {
    int y;
    int x;
    int w = above->width;
    int ha = above->height;
    long long sum = 0;
    int pixels = 0;
    int skip = (int)(overlap * SC_HEADER_SKIP_FRAC);
    int check_rows = overlap - skip;

    if (check_rows < 8) {
        skip = 0;
        check_rows = overlap;
    }

    for (y = skip; y < overlap; y++) {
        const unsigned char *ra = above->rgb + (size_t)(ha - overlap + y) * (size_t)w * 3u;
        const unsigned char *rb = below->rgb + (size_t)y * (size_t)w * 3u;
        for (x = 0; x < w; x += 2) {
            sum += abs((int)ra[x * 3] - (int)rb[x * 3]);
            sum += abs((int)ra[x * 3 + 1] - (int)rb[x * 3 + 1]);
            sum += abs((int)ra[x * 3 + 2] - (int)rb[x * 3 + 2]);
            pixels += 3;
        }
    }

    if (pixels == 0) {
        return 1.0;
    }
    return (sum / (double)pixels) / 255.0;
}

static double strip_ssd(
    const MatchData *above,
    const MatchData *below,
    int overlap,
    int x0,
    int x1,
    int skip_top
) {
    int y;
    int x;
    double ssd = 0.0;
    int pixels = 0;

    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > above->width) {
        x1 = above->width;
    }
    if (x0 >= x1) {
        return 1e9;
    }

    for (y = skip_top; y < overlap; y++) {
        const float *row_a = above->gray + (above->height - overlap + y) * above->width;
        const float *row_b = below->gray + y * below->width;
        for (x = x0; x < x1; x++) {
            ssd += fabs((double)row_a[x] - (double)row_b[x]);
            pixels++;
        }
    }
    return pixels > 0 ? ssd / (double)pixels : 1e9;
}

static double overlap_cost(
    const MatchData *above,
    const MatchData *below,
    int overlap,
    int preferred_overlap,
    int has_preferred,
    int skip_top
) {
    int w = above->width;
    int s1 = w / 4;
    int s2 = w / 2;
    double ssd_left = strip_ssd(above, below, overlap, 0, s1, skip_top);
    double ssd_mid = strip_ssd(above, below, overlap, s1, s2, skip_top);
    double ssd_right = strip_ssd(above, below, overlap, s2, w, skip_top);
    double ssd = (ssd_left + ssd_mid + ssd_right) / 3.0;
    float ncc_int;
    float ncc_grad;
    double cost;

    ncc_int = ncc(
        above->intensity_profile + above->height - overlap,
        below->intensity_profile,
        overlap
    );
    ncc_grad = ncc(
        above->gradient_profile + above->height - overlap,
        below->gradient_profile,
        overlap
    );

    cost = ssd - 45.0 * (double)ncc_int - 25.0 * (double)ncc_grad;

    if (has_preferred) {
        if (overlap > preferred_overlap) {
            cost += 0.25 * (double)(overlap - preferred_overlap);
        } else {
            cost += 0.12 * (double)(preferred_overlap - overlap);
        }
    }
    return cost;
}

static int refine_overlap_by_seam(
    const ScImage *above,
    const ScImage *below,
    int overlap,
    int min_overlap,
    int max_overlap,
    int expected_overlap
) {
    double seam = seam_mean_diff(above, below, overlap);
    int best = overlap;
    int delta;

    if (seam <= SC_SEAM_GOOD) {
        return overlap;
    }

    for (delta = 2; delta <= 50; delta += 2) {
        int up = overlap + delta;
        if (up <= max_overlap) {
            double s = seam_mean_diff(above, below, up);
            if (s < seam) {
                seam = s;
                best = up;
            }
        }
        if (seam <= SC_SEAM_GOOD) {
            return best;
        }
    }

    if (seam > SC_SEAM_BAD) {
        for (delta = 2; delta <= 30; delta += 2) {
            int down = overlap - delta;
            if (down >= min_overlap) {
                double s = seam_mean_diff(above, below, down);
                if (s < seam) {
                    seam = s;
                    best = down;
                }
            }
            if (seam <= SC_SEAM_GOOD) {
                return best;
            }
        }
    }

    if (seam > SC_SEAM_BAD && expected_overlap >= min_overlap && expected_overlap <= max_overlap) {
        double s = seam_mean_diff(above, below, expected_overlap);
        if (s < seam) {
            best = expected_overlap;
            seam = s;
        }
    }

    if (seam > SC_SEAM_BAD) {
        printf("  [warn] weak seam match (%.3f), overlap=%d px\n", seam, best);
    }

    return best;
}

int sc_overlap_search_bounds(
    int frame_height,
    int wheel_notches,
    int expected_overlap,
    int preferred_overlap,
    int has_preferred,
    int *min_overlap,
    int *max_overlap
) {
    int estimated_scroll = wheel_notches * SC_PX_PER_WHEEL_NOTCH;
    int min_o;
    int max_o;

    if (estimated_scroll < 40) {
        estimated_scroll = 40;
    }

    if (has_preferred) {
        min_o = preferred_overlap - 22;
        max_o = preferred_overlap + 18;
    } else if (expected_overlap > 0) {
        min_o = expected_overlap - 70;
        max_o = expected_overlap + 35;
    } else {
        int min_scroll = (int)(estimated_scroll * 0.82);
        int max_scroll = (int)(estimated_scroll * 1.08);
        min_o = frame_height - max_scroll;
        max_o = frame_height - min_scroll;
    }

    if (min_o < 40) {
        min_o = 40;
    }
    if (max_o > frame_height - 30) {
        max_o = frame_height - 30;
    }
    if (min_o >= max_o) {
        min_o = 40;
        max_o = frame_height - 30;
    }

    *min_overlap = min_o;
    *max_overlap = max_o;
    return 1;
}

ScOverlapMatch sc_find_vertical_overlap(
    const ScImage *above,
    const ScImage *below,
    int min_overlap,
    int max_overlap,
    int preferred_overlap,
    int has_preferred
) {
    MatchData ma;
    MatchData mb;
    ScOverlapMatch result;
    int overlap;
    int best_overlap;
    double best_cost;
    int fine_min;
    int fine_max;
    int skip_top;
    int expected = has_preferred ? preferred_overlap : (min_overlap + max_overlap) / 2;

    result.overlap = 20;
    result.score = 1.0;

    if (!match_data_prepare(above, &ma) || !match_data_prepare(below, &mb)) {
        return result;
    }

    skip_top = (int)(ma.height * SC_HEADER_SKIP_FRAC);
    if (skip_top < 6) {
        skip_top = 6;
    }

    {
        int height = ma.height < mb.height ? ma.height : mb.height;
        if (max_overlap > height - 20) {
            max_overlap = height - 20;
        }
        if (max_overlap > ma.height - 10) {
            max_overlap = ma.height - 10;
        }
        if (max_overlap > mb.height - 10) {
            max_overlap = mb.height - 10;
        }
    }

    if (min_overlap < skip_top + 10) {
        min_overlap = skip_top + 10;
    }
    if (min_overlap >= max_overlap) {
        result.overlap = min_overlap < max_overlap ? min_overlap : max_overlap;
        match_data_free(&ma);
        match_data_free(&mb);
        return result;
    }

    best_overlap = min_overlap;
    best_cost = 1e300;

    for (overlap = min_overlap; overlap <= max_overlap; overlap += 2) {
        double cost = overlap_cost(&ma, &mb, overlap, preferred_overlap, has_preferred, skip_top);
        if (cost < best_cost) {
            best_cost = cost;
            best_overlap = overlap;
        }
    }

    fine_min = best_overlap - 10;
    fine_max = best_overlap + 10;
    if (fine_min < min_overlap) {
        fine_min = min_overlap;
    }
    if (fine_max > max_overlap) {
        fine_max = max_overlap;
    }

    for (overlap = fine_min; overlap <= fine_max; overlap++) {
        double cost = overlap_cost(&ma, &mb, overlap, preferred_overlap, has_preferred, skip_top);
        if (cost < best_cost) {
            best_cost = cost;
            best_overlap = overlap;
        }
    }

    best_overlap = refine_overlap_by_seam(above, below, best_overlap, min_overlap, max_overlap, expected);

    result.overlap = best_overlap;
    result.score = best_cost;

    match_data_free(&ma);
    match_data_free(&mb);
    return result;
}

int sc_stabilize_overlaps(int *overlaps, int count) {
    int *copy;
    int median;
    int i;

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
        if (abs(overlaps[i] - median) > 50) {
            overlaps[i] = median;
        } else if (overlaps[i] > median + 20) {
            overlaps[i] = median + 8;
        } else if (overlaps[i] < median - 20) {
            overlaps[i] = median - 8;
        }
    }

    for (i = 0; i < count; i++) {
        int start = i - 1;
        int end = i + 2;
        int window[3];
        int wcount = 0;
        int j;

        if (start < 0) {
            start = 0;
        }
        if (end > count) {
            end = count;
        }

        for (j = start; j < end; j++) {
            window[wcount++] = overlaps[j];
        }
        qsort(window, (size_t)wcount, sizeof(int), compare_int);
        overlaps[i] = window[wcount / 2];
    }

    free(copy);
    return count;
}

ScImage *sc_stitch_frames(const ScFrameList *frames, const int *overlaps, int overlap_count) {
    int i;
    int total_height = 0;
    int overlap;
    ScImage *result;
    unsigned char *dst;
    int y = 0;

    if (!frames || frames->count == 0) {
        return NULL;
    }
    if (frames->count == 1) {
        return sc_image_copy(frames->items[0]);
    }
    if (!overlaps || overlap_count != frames->count - 1) {
        return NULL;
    }

    total_height = frames->items[0]->height;
    for (i = 1; i < frames->count; i++) {
        overlap = overlaps[i - 1];
        if (overlap < 1) {
            overlap = 1;
        }
        if (overlap > frames->items[i]->height - 1) {
            overlap = frames->items[i]->height - 1;
        }
        total_height += frames->items[i]->height - overlap;
    }

    result = sc_image_create(frames->items[0]->width, total_height);
    if (!result) {
        return NULL;
    }

    dst = result->rgb;
    memcpy(dst, frames->items[0]->rgb, (size_t)frames->items[0]->width * (size_t)frames->items[0]->height * 3u);
    y = frames->items[0]->height;

    for (i = 1; i < frames->count; i++) {
        int src_h;
        int row_bytes;
        int src_y;
        overlap = overlaps[i - 1];
        if (overlap < 1) {
            overlap = 1;
        }
        if (overlap > frames->items[i]->height - 1) {
            overlap = frames->items[i]->height - 1;
        }

        src_h = frames->items[i]->height - overlap;
        row_bytes = frames->items[i]->width * 3;
        for (src_y = 0; src_y < src_h; src_y++) {
            memcpy(
                dst + (size_t)(y + src_y) * (size_t)row_bytes,
                frames->items[i]->rgb + (size_t)(overlap + src_y) * (size_t)row_bytes,
                (size_t)row_bytes
            );
        }
        y += src_h;
    }

    return result;
}
