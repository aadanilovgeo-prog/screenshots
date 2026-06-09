#include "scroll_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_STABLE_ATTEMPTS 10
#define SC_STABLE_INTERVAL_MS 150
#define SC_STABLE_DIFF 0.0035

int sc_wait_for_frame_stable(const ScRegion *region, ScImage *out) {
    ScImage *a;
    ScImage *b;
    size_t bytes;
    int attempt;

    if (!region || !out) {
        return 0;
    }

    a = sc_image_create(region->width, region->height);
    b = sc_image_create(region->width, region->height);
    if (!a || !b) {
        sc_image_free(a);
        sc_image_free(b);
        return sc_capture_region(region, out);
    }

    bytes = (size_t)region->width * (size_t)region->height * 3u;
    if (!sc_capture_region(region, b)) {
        sc_image_free(a);
        sc_image_free(b);
        return 0;
    }

    for (attempt = 0; attempt < SC_STABLE_ATTEMPTS; attempt++) {
        sc_sleep_ms(SC_STABLE_INTERVAL_MS);
        if (!sc_capture_region(region, a)) {
            break;
        }
        if (sc_image_diff_ratio(a, b) < SC_STABLE_DIFF) {
            sc_sleep_ms(80);
            if (!sc_capture_region(region, a)) {
                break;
            }
            if (sc_image_diff_ratio(a, b) < SC_STABLE_DIFF) {
                memcpy(out->rgb, a->rgb, bytes);
                sc_image_free(a);
                sc_image_free(b);
                return 1;
            }
        }
        memcpy(b->rgb, a->rgb, bytes);
    }

    memcpy(out->rgb, a->rgb, bytes);
    sc_image_free(a);
    sc_image_free(b);
    return 1;
}

int sc_capture_long_page(
    const ScRegion *region,
    const ScScrollSettings *scroll,
    double scroll_delay,
    double settle_delay,
    int max_frames,
    double same_frame_threshold,
    int safe_stitch,
    const char *save_frames_dir,
    ScFrameList *frames,
    int *crops,
    int *crop_count,
    int *reached_end,
    ScStitchLog *log
) {
    ScImage *previous = NULL;
    ScImage *current = NULL;
    int index;
    int overlap_capacity = max_frames;
    char frame_path[600];
    char preview_path[600];

    if (!region || !scroll || !frames || !crops || !crop_count || !reached_end) {
        return 0;
    }

    *crop_count = 0;
    *reached_end = 0;

    if (scroll->focus_click) {
        printf("Focusing article region (mouse click)...\n");
        sc_focus_region(region);
        sc_sleep_ms(250);
    }

    previous = sc_image_create(region->width, region->height);
    if (!previous) {
        return 0;
    }
    if (!sc_wait_for_frame_stable(region, previous)) {
        sc_image_free(previous);
        return 0;
    }

    if (!sc_frame_list_push(frames, previous)) {
        sc_image_free(previous);
        return 0;
    }

    if (save_frames_dir) {
        snprintf(frame_path, sizeof(frame_path), "%s/frame_0000.png", save_frames_dir);
        sc_save_png(frame_path, previous);
    }

    printf("Capturing first frame (safe stitch=%s)...\n", safe_stitch ? "on" : "off");

    for (index = 1; index <= max_frames; index++) {
        ScShiftData shift;
        ScSafeCrop safe_crop;
        double diff;

        sc_scroll_wheel_at(region, scroll);
        sc_sleep_ms((int)(scroll_delay * 1000.0));

        current = sc_image_create(region->width, region->height);
        if (!current) {
            return 0;
        }
        if (!sc_wait_for_frame_stable(region, current)) {
            sc_image_free(current);
            return 0;
        }
        sc_sleep_ms((int)(settle_delay * 1000.0));

        diff = sc_image_diff_ratio(previous, current);
        if (diff <= same_frame_threshold) {
            printf("End of page reached (frames are nearly identical, diff=%.3f%%).\n", diff * 100.0);
            *reached_end = 1;
            sc_image_free(current);
            break;
        }

        if (!sc_detect_vertical_content_shift(previous, current, &shift)) {
            fprintf(stderr, "Failed to detect content shift for frame %d.\n", index);
            sc_image_free(current);
            return 0;
        }

        safe_crop = sc_choose_safe_crop(previous, current, &shift, safe_stitch);
        crops[*crop_count] = safe_crop.crop;
        (*crop_count)++;

        sc_stitch_log_frame(log, index, &shift, &safe_crop, 0);

        if (save_frames_dir) {
            snprintf(frame_path, sizeof(frame_path), "%s/frame_%04d.png", save_frames_dir, index);
            sc_save_png(frame_path, current);
            snprintf(preview_path, sizeof(preview_path), "%s/seam_%04d_preview.png", save_frames_dir, index);
            sc_save_seam_preview(preview_path, previous, current, safe_crop.crop);
        }

        if (!sc_frame_list_push(frames, current)) {
            sc_image_free(current);
            return 0;
        }

        previous = current;

        if (*crop_count >= overlap_capacity) {
            break;
        }
    }

    return 1;
}
