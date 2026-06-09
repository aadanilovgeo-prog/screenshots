#include "scroll_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_STABLE_ATTEMPTS 10
#define SC_STABLE_INTERVAL_MS 150
#define SC_STABLE_DIFF 0.0035
#define SC_MIN_ACCEPT_NEW_FRAC 0.08

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

static int adaptive_scroll_to_target(
    const ScRegion *region,
    const ScScrollSettings *scroll,
    const ScImage *previous,
    ScImage *current,
    ScShiftData *shift,
    double same_frame_threshold
) {
    int height = region->height;
    int min_shift;
    int max_shift;
    int micro;
    double diff;

    if (!region || !scroll || !previous || !current || !shift) {
        return 0;
    }

    min_shift = (int)(height * scroll->min_new_frac);
    max_shift = (int)(height * scroll->max_new_frac);
    if (min_shift < 24) {
        min_shift = 24;
    }
    if (max_shift <= min_shift) {
        max_shift = min_shift + height / 10;
    }

    memset(shift, 0, sizeof(*shift));

    for (micro = 0; micro < scroll->max_micro_steps; micro++) {
        sc_scroll_one_notch(region, scroll);
        sc_sleep_ms((int)(scroll->micro_delay * 1000.0));

        if (!sc_wait_for_frame_stable(region, current)) {
            return 0;
        }

        diff = sc_image_diff_ratio(previous, current);
        if (diff <= same_frame_threshold) {
            return 2;
        }

        if (!sc_detect_vertical_content_shift(previous, current, shift)) {
            return 0;
        }

        shift->micro_steps_used = micro + 1;
        shift->new_content_frac = (double)shift->detected_shift / (double)height;

        printf(
            "  Adaptive scroll micro=%d shift=%dpx (%.1f%% new content, target %.0f-%.0f%%)\n",
            micro + 1,
            shift->detected_shift,
            shift->new_content_frac * 100.0,
            scroll->min_new_frac * 100.0,
            scroll->max_new_frac * 100.0
        );

        if (shift->detected_shift >= min_shift && shift->detected_shift <= max_shift) {
            return 1;
        }

        if (shift->detected_shift > max_shift) {
            shift->scroll_overshoot = 1;
            shift->confidence *= 0.65;
            printf(
                "  [warn] Scroll overshoot (%.1f%% > %.0f%%), accepting frame with lower confidence\n",
                shift->new_content_frac * 100.0,
                scroll->max_new_frac * 100.0
            );
            return 1;
        }
    }

    if (shift->detected_shift >= (int)(height * SC_MIN_ACCEPT_NEW_FRAC)) {
        shift->confidence *= 0.75;
        printf(
            "  [warn] Max micro-steps reached at %.1f%% new content (target %.0f-%.0f%%)\n",
            shift->new_content_frac * 100.0,
            scroll->min_new_frac * 100.0,
            scroll->max_new_frac * 100.0
        );
        return 1;
    }

    fprintf(
        stderr,
        "Scroll did not move enough after %d micro-steps (shift=%dpx, need >=%dpx).\n",
        scroll->max_micro_steps,
        shift->detected_shift,
        min_shift
    );
    return 0;
}

int sc_capture_long_page(
    const ScRegion *region,
    const ScScrollSettings *scroll,
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

    printf(
        "Capturing first frame (adaptive scroll %.0f-%.0f%% new content, safe stitch=%s)...\n",
        scroll->min_new_frac * 100.0,
        scroll->max_new_frac * 100.0,
        safe_stitch ? "on" : "off"
    );

    for (index = 1; index <= max_frames; index++) {
        ScShiftData shift;
        ScSafeCrop safe_crop;
        int scroll_result;

        current = sc_image_create(region->width, region->height);
        if (!current) {
            return 0;
        }

        scroll_result = adaptive_scroll_to_target(
            region,
            scroll,
            previous,
            current,
            &shift,
            same_frame_threshold
        );

        if (scroll_result == 2) {
            printf("End of page reached (no content change after scroll).\n");
            *reached_end = 1;
            sc_image_free(current);
            break;
        }
        if (scroll_result == 0) {
            sc_image_free(current);
            return 0;
        }

        {
            int saved_micro = shift.micro_steps_used;
            int saved_overshoot = shift.scroll_overshoot;

            sc_sleep_ms((int)(settle_delay * 1000.0));
            if (!sc_wait_for_frame_stable(region, current)) {
                sc_image_free(current);
                return 0;
            }

            if (!sc_detect_vertical_content_shift(previous, current, &shift)) {
                fprintf(stderr, "Failed to detect content shift for frame %d.\n", index);
                sc_image_free(current);
                return 0;
            }

            shift.micro_steps_used = saved_micro;
            shift.new_content_frac = (double)shift.detected_shift / (double)region->height;
            if (saved_overshoot) {
                shift.scroll_overshoot = 1;
                shift.confidence *= 0.65;
            }
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
