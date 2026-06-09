#include "scroll_capture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_HEADER_SKIP_FRAC 0.10
#define SC_SEARCH_BAND_FRAC 0.50
#define SC_MIN_NEW_CONTENT 24
#define SC_MIN_CROP 16
#define SC_CONFIDENCE_GOOD 0.62
#define SC_CONFIDENCE_WEAK 0.42
#define SC_SEAM_GOOD 0.040
#define SC_SEAM_WEAK 0.080

typedef struct {
    float *gray;
    float *intensity;
    float *gradient;
    float *edge;
    int width;
    int height;
} FrameFeatures;

static const int CROP_OFFSETS_SAFE[] = { -80, -50, -30, -20, -10, 0 };
static const int CROP_OFFSETS_ALL[] = { -80, -50, -30, -20, -10, 0, 10, 20 };


static float ncc_rows(const float *a, const float *b, int len) {
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

static void features_free(FrameFeatures *f) {
    if (!f) {
        return;
    }
    free(f->gray);
    free(f->intensity);
    free(f->gradient);
    free(f->edge);
    memset(f, 0, sizeof(*f));
}

static int features_prepare(const ScImage *img, FrameFeatures *f) {
    int x;
    int y;
    int margin_x;
    int cropped_w;

    if (!img || !img->rgb || !f) {
        return 0;
    }

    memset(f, 0, sizeof(*f));
    f->height = img->height;
    f->width = img->width;

    margin_x = img->width / 10;
    if (margin_x < 8) {
        margin_x = 8;
    }
    cropped_w = img->width - margin_x * 2;
    if (cropped_w < 16) {
        return 0;
    }

    f->intensity = (float *)malloc((size_t)img->height * sizeof(float));
    f->gradient = (float *)malloc((size_t)img->height * sizeof(float));
    f->edge = (float *)malloc((size_t)img->height * sizeof(float));
    f->gray = (float *)malloc((size_t)cropped_w * (size_t)img->height * sizeof(float));

    if (!f->intensity || !f->gradient || !f->edge || !f->gray) {
        features_free(f);
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
            f->gray[y * cropped_w + x] = lum;
            row_sum += lum;
            if (has_prev) {
                grad_sum += fabsf(lum - prev);
            }
            prev = lum;
            has_prev = 1;
        }

        f->intensity[y] = row_sum / (float)cropped_w;
        f->gradient[y] = grad_sum / (float)cropped_w;
    }

    f->edge[0] = f->gradient[0];
    for (y = 1; y < img->height; y++) {
        f->edge[y] = fabsf(f->intensity[y] - f->intensity[y - 1]) + f->gradient[y] * 0.35f;
    }

    f->width = cropped_w;
    return 1;
}

static int overlap_band_rows(
    int crop,
    int height,
    int *y_start,
    int *y_end,
    int skip_top
) {
    int band_lo = (int)(height * (1.0 - SC_SEARCH_BAND_FRAC));
    int band_hi = (int)(height * SC_SEARCH_BAND_FRAC);
    int overlap_lo = height - crop;
    int start = overlap_lo > band_lo ? overlap_lo : band_lo;
    int end = crop < band_hi ? crop : band_hi;

    start += skip_top;
    if (start < overlap_lo + skip_top) {
        start = overlap_lo + skip_top;
    }
    if (end > crop) {
        end = crop;
    }
    if (end - start < 12) {
        start = skip_top;
        end = crop;
    }
    *y_start = start;
    *y_end = end;
    return end > start;
}

static double band_ssd_rows(
    const FrameFeatures *above,
    const FrameFeatures *below,
    int crop,
    int y_start,
    int y_end,
    int x0,
    int x1
) {
    int y;
    int x;
    double ssd = 0.0;
    int pixels = 0;
    int ha = above->height;
    int row_a_start = ha - crop;

    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > above->width) {
        x1 = above->width;
    }
    if (x0 >= x1 || y_end <= y_start) {
        return 1e9;
    }

    for (y = y_start; y < y_end; y++) {
        int ya = row_a_start + y;
        const float *row_a = above->gray + ya * above->width;
        const float *row_b = below->gray + y * below->width;
        for (x = x0; x < x1; x++) {
            ssd += fabs((double)row_a[x] - (double)row_b[x]);
            pixels++;
        }
    }

    return pixels > 0 ? ssd / (double)pixels : 1e9;
}

static double crop_match_cost(
    const FrameFeatures *above,
    const FrameFeatures *below,
    int crop,
    int skip_top
) {
    int ha = above->height;
    int hb = below->height;
    int w = above->width;
    int s1 = w / 4;
    int s2 = w / 2;
    int y_start;
    int y_end;
    double ssd;
    float ncc_i;
    float ncc_g;
    float ncc_e;
    double crop_frac;
    double penalty;

    if (crop < skip_top + 8 || crop >= ha - SC_MIN_NEW_CONTENT || crop >= hb - 4) {
        return 1e9;
    }
    if (!overlap_band_rows(crop, ha, &y_start, &y_end, skip_top)) {
        return 1e9;
    }

    ssd = (
        band_ssd_rows(above, below, crop, y_start, y_end, 0, s1) +
        band_ssd_rows(above, below, crop, y_start, y_end, s1, s2) +
        band_ssd_rows(above, below, crop, y_start, y_end, s2, w)
    ) / 3.0;

    ncc_i = ncc_rows(above->intensity + ha - crop + y_start, below->intensity + y_start, y_end - y_start);
    ncc_g = ncc_rows(above->gradient + ha - crop + y_start, below->gradient + y_start, y_end - y_start);
    ncc_e = ncc_rows(above->edge + ha - crop + y_start, below->edge + y_start, y_end - y_start);

    crop_frac = (double)crop / (double)ha;
    penalty = 0.0;
    if (crop_frac > 0.72) {
        penalty += (crop_frac - 0.72) * 40.0;
    }

    return ssd - 50.0 * (double)ncc_i - 28.0 * (double)ncc_g - 18.0 * (double)ncc_e + penalty;
}


double sc_evaluate_seam(const ScImage *above, const ScImage *below, int crop) {
    int y;
    int x;
    int w = above->width;
    int ha = above->height;
    long long sum = 0;
    int pixels = 0;
    int skip = (int)(crop * SC_HEADER_SKIP_FRAC);
    int check_rows = crop - skip;

    if (crop < 8) {
        return 1.0;
    }
    if (check_rows < 8) {
        skip = 0;
        check_rows = crop;
    }

    for (y = skip; y < crop; y++) {
        const unsigned char *ra = above->rgb + (size_t)(ha - crop + y) * (size_t)w * 3u;
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

static double table_seam_risk(const FrameFeatures *above, const FrameFeatures *below, int crop) {
    int ha = above->height;
    int y;
    double edge_above = 0.0;
    double edge_below = 0.0;
    double edge_seam = 0.0;
    int count = 0;

    for (y = crop - 6; y < crop + 6; y++) {
        if (y >= 1 && y < ha) {
            edge_above += (double)above->edge[y];
            count++;
        }
    }
    for (y = crop - 6; y < crop + 6; y++) {
        if (y >= 0 && y < below->height) {
            edge_below += (double)below->edge[y];
        }
    }
    if (crop >= 0 && crop < below->height) {
        edge_seam = (double)below->edge[crop];
    }

    if (count <= 0) {
        return 0.5;
    }

    edge_above /= (double)count;
    edge_below /= (double)count;

    if (edge_seam > (edge_above + edge_below) * 0.55 + 0.8) {
        return fmin(1.0, (edge_seam - edge_above) / 8.0);
    }
    return 0.05;
}

static double image_seam_risk(
    const FrameFeatures *above,
    const FrameFeatures *below,
    int crop,
    double seam_diff
) {
    int ha = above->height;
    int y;
    double local_grad = 0.0;
    int count = 0;

    for (y = ha - crop - 8; y < ha - crop + 8; y++) {
        if (y >= 0 && y < ha) {
            local_grad += (double)above->gradient[y];
            count++;
        }
    }
    for (y = crop - 8; y < crop + 8; y++) {
        if (y >= 0 && y < below->height) {
            local_grad += (double)below->gradient[y];
            count++;
        }
    }

    if (count <= 0) {
        return seam_diff;
    }

    local_grad /= (double)count;
    if (local_grad > 6.0 && seam_diff > SC_SEAM_GOOD) {
        return fmin(1.0, seam_diff + local_grad / 40.0);
    }
    return seam_diff * 0.6;
}

static double estimate_confidence(double best_cost, double second_cost, double seam_diff) {
    double gap = second_cost - best_cost;
    double conf = 0.35 + fmin(0.45, gap / 8.0);
    if (seam_diff <= SC_SEAM_GOOD) {
        conf += 0.18;
    } else if (seam_diff <= SC_SEAM_WEAK) {
        conf += 0.06;
    } else {
        conf -= 0.12;
    }
    if (conf < 0.0) {
        conf = 0.0;
    }
    if (conf > 1.0) {
        conf = 1.0;
    }
    return conf;
}

int sc_detect_vertical_content_shift(
    const ScImage *above,
    const ScImage *below,
    ScShiftData *out
) {
    FrameFeatures fa;
    FrameFeatures fb;
    int height;
    int min_crop;
    int max_crop;
    int skip_top;
    int crop;
    int best_crop;
    int second_crop;
    double best_cost;
    double second_cost;
    double cost;
    int fine_min;
    int fine_max;

    if (!above || !below || !out) {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    height = above->height < below->height ? above->height : below->height;

    if (!features_prepare(above, &fa) || !features_prepare(below, &fb)) {
        features_free(&fa);
        features_free(&fb);
        return 0;
    }

    skip_top = (int)(height * SC_HEADER_SKIP_FRAC);
    if (skip_top < 6) {
        skip_top = 6;
    }

    min_crop = SC_MIN_CROP;
    max_crop = height - SC_MIN_NEW_CONTENT;
    if (max_crop <= min_crop + 20) {
        min_crop = height / 6;
        max_crop = height - 8;
    }

    best_crop = min_crop;
    second_crop = min_crop + 2;
    best_cost = 1e300;
    second_cost = 1e300;

    for (crop = min_crop; crop <= max_crop; crop += 2) {
        cost = crop_match_cost(&fa, &fb, crop, skip_top);
        if (cost < best_cost) {
            second_cost = best_cost;
            second_crop = best_crop;
            best_cost = cost;
            best_crop = crop;
        } else if (cost < second_cost) {
            second_cost = cost;
            second_crop = crop;
        }
    }

    fine_min = best_crop - 16;
    fine_max = best_crop + 16;
    if (fine_min < min_crop) {
        fine_min = min_crop;
    }
    if (fine_max > max_crop) {
        fine_max = max_crop;
    }

    for (crop = fine_min; crop <= fine_max; crop++) {
        cost = crop_match_cost(&fa, &fb, crop, skip_top);
        if (cost < best_cost) {
            second_cost = best_cost;
            second_crop = best_crop;
            best_cost = cost;
            best_crop = crop;
        } else if (cost < second_cost) {
            second_cost = cost;
            second_crop = crop;
        }
    }

    {
        double seam = sc_evaluate_seam(above, below, best_crop);
        out->detected_shift = height - best_crop;
        out->detected_overlap = best_crop;
        out->initial_crop = best_crop;
        out->match_score = best_cost;
        out->confidence = estimate_confidence(best_cost, second_cost, seam);
        (void)second_crop;
    }

    features_free(&fa);
    features_free(&fb);
    return 1;
}

ScSafeCrop sc_choose_safe_crop(
    const ScImage *above,
    const ScImage *below,
    const ScShiftData *shift,
    int safe_mode
) {
    FrameFeatures fa;
    FrameFeatures fb;
    ScSafeCrop result;
    int height;
    int min_crop;
    int max_crop;
    int i;
    int best_idx = 0;
    double best_key = 1e300;
    int initial;

    memset(&result, 0, sizeof(result));
    if (!above || !below || !shift) {
        result.crop = SC_MIN_CROP;
        return result;
    }

    initial = shift->initial_crop > 0 ? shift->initial_crop : shift->detected_overlap;
    if (initial < SC_MIN_CROP) {
        initial = SC_MIN_CROP;
    }

    height = above->height < below->height ? above->height : below->height;
    min_crop = SC_MIN_CROP;
    max_crop = height - SC_MIN_NEW_CONTENT;
    if (max_crop <= min_crop) {
        max_crop = min_crop + 1;
    }

    if (!features_prepare(above, &fa) || !features_prepare(below, &fb)) {
        result.crop = initial;
        result.initial_crop = initial;
        result.confidence = shift->confidence;
        result.safe_mode = safe_mode;
        result.content_loss_risk = 0.5;
        result.duplicate_risk = 0.5;
        features_free(&fa);
        features_free(&fb);
        return result;
    }

    result.initial_crop = initial;
    result.confidence = shift->confidence;
    result.safe_mode = safe_mode;

    {
        const int *offsets = safe_mode ? CROP_OFFSETS_SAFE : CROP_OFFSETS_ALL;
        int offset_count = safe_mode
            ? (int)(sizeof(CROP_OFFSETS_SAFE) / sizeof(CROP_OFFSETS_SAFE[0]))
            : (int)(sizeof(CROP_OFFSETS_ALL) / sizeof(CROP_OFFSETS_ALL[0]));

    for (i = 0; i < offset_count; i++) {
        int candidate = initial + offsets[i];
        double seam;
        double content_loss;
        double duplicate;
        double table_risk;
        double image_risk;
        double key;
        int weak;

        if (candidate < min_crop) {
            candidate = min_crop;
        }
        if (candidate > max_crop) {
            candidate = max_crop;
        }

        seam = sc_evaluate_seam(above, below, candidate);
        weak = seam > SC_SEAM_WEAK;

        content_loss = (double)(candidate - min_crop) / (double)(max_crop - min_crop + 1);
        duplicate = (double)(max_crop - candidate) / (double)(max_crop - min_crop + 1);

        if (candidate > initial) {
            content_loss += 0.08 * (double)(candidate - initial) / (double)height;
        }
        if (candidate < initial) {
            duplicate += 0.06 * (double)(initial - candidate) / (double)height;
        }

        table_risk = table_seam_risk(&fa, &fb, candidate);
        image_risk = image_seam_risk(&fa, &fb, candidate, seam);

        if (weak) {
            content_loss += 0.12;
        }
        if (safe_mode) {
            if (shift->confidence < SC_CONFIDENCE_WEAK) {
                content_loss += 0.15 * (double)(candidate - min_crop) / (double)height;
                duplicate -= 0.04;
            } else if (shift->confidence < SC_CONFIDENCE_GOOD) {
                content_loss += 0.08 * (double)(candidate - initial) / (double)height;
            }
            if (table_risk > 0.35 || image_risk > 0.25) {
                content_loss += 0.10;
                duplicate -= 0.05;
            }
        }

        key = content_loss * 1000.0 + table_risk * 120.0 + image_risk * 150.0 + duplicate * 40.0 + seam * 80.0;

        if (safe_mode && shift->confidence < SC_CONFIDENCE_GOOD) {
            if (candidate > initial) {
                key += 25.0 * (double)(candidate - initial);
            }
        }

        if (key < best_key) {
            best_key = key;
            best_idx = i;
            result.crop = candidate;
            result.content_loss_risk = content_loss;
            result.duplicate_risk = duplicate;
            result.table_seam_risk = table_risk;
            result.image_seam_risk = image_risk;
            result.weak_seam = weak;
        }
    }

    if (safe_mode && result.confidence < SC_CONFIDENCE_WEAK && result.crop > initial) {
        result.crop = initial;
        result.content_loss_risk = fmin(result.content_loss_risk, 0.35);
        result.duplicate_risk = fmax(result.duplicate_risk, 0.25);
    }

    if (safe_mode && result.weak_seam && result.crop > initial - 10) {
        result.crop = initial - 10;
        if (result.crop < min_crop) {
            result.crop = min_crop;
        }
        result.duplicate_risk = fmax(result.duplicate_risk, 0.20);
    }

    }
    (void)best_idx;
    features_free(&fa);
    features_free(&fb);
    return result;
}

ScImage *sc_append_frame_safely(const ScImage *result, const ScImage *frame, int safe_crop) {
    return sc_image_append_crop(result, frame, safe_crop);
}

ScImage *sc_stitch_frames_safe(const ScFrameList *frames, const int *crops, int crop_count) {
    ScImage *result = NULL;
    int i;

    if (!frames || frames->count == 0) {
        return NULL;
    }
    if (frames->count == 1) {
        return sc_image_copy(frames->items[0]);
    }
    if (!crops || crop_count != frames->count - 1) {
        return NULL;
    }

    result = sc_image_copy(frames->items[0]);
    if (!result) {
        return NULL;
    }

    for (i = 1; i < frames->count; i++) {
        ScImage *next = sc_append_frame_safely(result, frames->items[i], crops[i - 1]);
        if (!next) {
            sc_image_free(result);
            return NULL;
        }
        sc_image_free(result);
        result = next;
    }

    return result;
}

void sc_stitch_log_init(ScStitchLog *log, const char *debug_dir) {
    char log_path[600];

    if (!log) {
        return;
    }

    memset(log, 0, sizeof(*log));
    if (!debug_dir || !debug_dir[0]) {
        return;
    }

    strncpy(log->debug_dir, debug_dir, sizeof(log->debug_dir) - 1);
    log->has_debug_dir = 1;
    sc_mkdir_p(debug_dir);

    snprintf(log_path, sizeof(log_path), "%s/stitch_log.txt", debug_dir);
    log->log_file = fopen(log_path, "w");
    if (log->log_file) {
        fprintf(log->log_file, "scroll_capture safe stitch log\n");
        fflush(log->log_file);
    }
}

void sc_stitch_log_close(ScStitchLog *log) {
    if (!log) {
        return;
    }
    if (log->log_file) {
        fclose(log->log_file);
        log->log_file = NULL;
    }
}

void sc_stitch_log_frame(
    ScStitchLog *log,
    int frame_index,
    const ScShiftData *shift,
    const ScSafeCrop *crop,
    int end_of_page
) {
    if (!log || !shift || !crop) {
        return;
    }

    printf(
        "  Frame %04d: micro=%d shift=%dpx (%.1f%% new) overlap=%dpx initial_crop=%d final_crop=%d "
        "confidence=%.2f safe_mode=%d dup_risk=%.3f loss_risk=%.3f "
        "table_risk=%.3f image_risk=%.3f%s\n",
        frame_index,
        shift->micro_steps_used,
        shift->detected_shift,
        shift->new_content_frac * 100.0,
        shift->detected_overlap,
        crop->initial_crop,
        crop->crop,
        crop->confidence,
        crop->safe_mode,
        crop->duplicate_risk,
        crop->content_loss_risk,
        crop->table_seam_risk,
        crop->image_seam_risk,
        crop->weak_seam ? " WEAK_SEAM" : ""
    );

    if (!log->log_file) {
        return;
    }

    fprintf(
        log->log_file,
        "Frame=%d\n"
        "MicroSteps=%d\n"
        "DetectedShift=%d\n"
        "NewContentFrac=%.4f\n"
        "DetectedOverlap=%d\n"
        "InitialCrop=%d\n"
        "FinalSafeCrop=%d\n"
        "Confidence=%.4f\n"
        "SafeMode=%d\n"
        "DuplicateRisk=%.4f\n"
        "ContentLossRisk=%.4f\n"
        "TableSeamRisk=%.4f\n"
        "ImageSeamRisk=%.4f\n"
        "WeakSeam=%d\n"
        "EndOfPage=%d\n\n",
        frame_index,
        shift->micro_steps_used,
        shift->detected_shift,
        shift->new_content_frac * 100.0,
        shift->detected_overlap,
        crop->initial_crop,
        crop->crop,
        crop->confidence,
        crop->safe_mode,
        crop->duplicate_risk,
        crop->content_loss_risk,
        crop->table_seam_risk,
        crop->image_seam_risk,
        crop->weak_seam,
        end_of_page
    );
    fflush(log->log_file);
}

int sc_save_seam_preview(
    const char *path,
    const ScImage *above,
    const ScImage *below,
    int crop
) {
    ScImage *preview;
    int w;
    int strip_h;
    int y;
    int row_bytes;

    if (!path || !above || !below || !above->rgb || !below->rgb) {
        return 0;
    }

    w = above->width;
    strip_h = 80;
    if (crop < strip_h) {
        strip_h = crop;
    }
    if (strip_h < 8) {
        strip_h = 8;
    }

    preview = sc_image_create(w, strip_h * 2 + 4);
    if (!preview) {
        return 0;
    }

    row_bytes = w * 3;
    for (y = 0; y < strip_h; y++) {
        int src_y = above->height - strip_h + y;
        if (src_y < 0) {
            src_y = 0;
        }
        memcpy(
            preview->rgb + (size_t)y * (size_t)row_bytes,
            above->rgb + (size_t)src_y * (size_t)row_bytes,
            (size_t)row_bytes
        );
    }

    memset(preview->rgb + (size_t)strip_h * (size_t)row_bytes, 255, (size_t)row_bytes * 4u);

    for (y = 0; y < strip_h; y++) {
        int src_y = crop - strip_h + y;
        if (src_y < 0) {
            src_y = y;
        }
        if (src_y >= below->height) {
            src_y = below->height - 1;
        }
        memcpy(
            preview->rgb + (size_t)(strip_h + 4 + y) * (size_t)row_bytes,
            below->rgb + (size_t)src_y * (size_t)row_bytes,
            (size_t)row_bytes
        );
    }

    {
        int ok = sc_save_png(path, preview);
        sc_image_free(preview);
        return ok;
    }
}
