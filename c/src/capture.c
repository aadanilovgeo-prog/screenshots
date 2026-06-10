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
    int have_stable = 0;

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
                have_stable = 1;
                break;
            }
        }
        memcpy(b->rgb, a->rgb, bytes);
    }

    if (!have_stable) {
        if (!sc_capture_region(region, out)) {
            sc_image_free(a);
            sc_image_free(b);
            return 0;
        }
    }

    sc_image_free(a);
    sc_image_free(b);
    return 1;
}

void sc_scroll_settings_init(const ScRegion *region, const ScConfig *cfg, ScScrollSettings *scroll) {
    (void)region;

    if (!scroll) {
        return;
    }

    scroll->micro_delay = cfg && cfg->micro_delay > 0.0 ? cfg->micro_delay : SC_MICRO_DELAY_SEC;
    scroll->max_micro_steps = SC_MAX_MICRO_STEPS;
    scroll->min_new_frac = SC_MIN_NEW_FRAC;
    scroll->max_new_frac = SC_MAX_NEW_FRAC;
    scroll->focus_click = cfg && !cfg->no_focus_click;
    scroll->focus_each_step = cfg && cfg->focus_each_step;
}

#define SC_SCROLL_END 2

static int adaptive_scroll_to_target(
    const ScRegion *region,
    const ScScrollSettings *scroll,
    const ScImage *previous,
    ScImage *current,
    ScShiftData *shift,
    double same_frame_threshold
) {
    int height;
    int min_shift;
    int max_shift;
    int micro;
    double diff;

    if (!region || !scroll || !previous || !current || !shift) {
        return 0;
    }

    height = region->height;
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
        sc_scroll_wheel_notches(region, scroll, 1);
        sc_sleep_ms((int)(scroll->micro_delay * 1000.0));

        if (!sc_wait_for_frame_stable(region, current)) {
            return 0;
        }

        diff = sc_image_diff_ratio(previous, current);
        if (diff <= same_frame_threshold) {
            return SC_SCROLL_END;
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
    ScImage **out_result,
    int *frames_captured,
    int *reached_end,
    int *memory_limit_hit,
    ScStitchLog *log
) {
    ScImage *previous = NULL;
    ScImage *current = NULL;
    ScImage *stitched = NULL;
    ScFrameList frames;
    int *crops = NULL;
    int crop_count = 0;
    int overlap_capacity;
    int index;
    char frame_path[600];
    char preview_path[600];

    if (!region || !scroll || !out_result || !frames_captured || !reached_end || !memory_limit_hit) {
        return 0;
    }

    *out_result = NULL;
    *frames_captured = 0;
    *reached_end = 0;
    *memory_limit_hit = 0;

    overlap_capacity = max_frames;
    if (overlap_capacity < 1) {
        overlap_capacity = 1;
    }

    crops = (int *)calloc((size_t)overlap_capacity, sizeof(int));
    if (!crops) {
        fprintf(stderr, "Out of memory.\n");
        return 0;
    }
    sc_frame_list_init(&frames);

    if (scroll->focus_click) {
        printf("Focusing article region (mouse click)...\n");
        sc_focus_region(region);
        sc_sleep_ms(250);
    }

    previous = sc_image_create(region->width, region->height);
    if (!previous) {
        goto fail;
    }
    if (!sc_wait_for_frame_stable(region, previous)) {
        goto fail;
    }

    if (!sc_frame_list_push(&frames, previous)) {
        goto fail;
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
            goto fail;
        }

        scroll_result = adaptive_scroll_to_target(
            region,
            scroll,
            previous,
            current,
            &shift,
            same_frame_threshold
        );

        if (scroll_result == SC_SCROLL_END) {
            printf("End of page reached (no content change after scroll).\n");
            *reached_end = 1;
            sc_image_free(current);
            current = NULL;
            break;
        }
        if (scroll_result == 0) {
            goto fail;
        }

        {
            int saved_micro = shift.micro_steps_used;
            int saved_overshoot = shift.scroll_overshoot;

            sc_sleep_ms((int)(settle_delay * 1000.0));
            if (!sc_wait_for_frame_stable(region, current)) {
                goto fail;
            }

            if (!sc_detect_vertical_content_shift(previous, current, &shift)) {
                fprintf(stderr, "Failed to detect content shift for frame %d.\n", index);
                goto fail;
            }

            shift.micro_steps_used = saved_micro;
            shift.new_content_frac = (double)shift.detected_shift / (double)region->height;
            if (saved_overshoot) {
                shift.scroll_overshoot = 1;
                shift.confidence *= 0.65;
            }
        }

        safe_crop = sc_choose_safe_crop(previous, current, &shift, safe_stitch);
        if (crop_count < overlap_capacity) {
            crops[crop_count] = safe_crop.crop;
            crop_count++;
        }

        sc_stitch_log_frame(log, index, &shift, &safe_crop, 0);

        if (save_frames_dir) {
            snprintf(frame_path, sizeof(frame_path), "%s/frame_%04d.png", save_frames_dir, index);
            sc_save_png(frame_path, current);
            snprintf(preview_path, sizeof(preview_path), "%s/seam_%04d_preview.png", save_frames_dir, index);
            sc_save_seam_preview(preview_path, previous, current, safe_crop.crop);
        }

        if (!sc_frame_list_push(&frames, current)) {
            goto fail;
        }
        previous = current;
        current = NULL;

        if (crop_count >= overlap_capacity) {
            break;
        }
    }

    if (frames.count < 1) {
        goto fail;
    }

    stitched = sc_stitch_frames_safe(&frames, crops, crop_count);
    if (!stitched) {
        fprintf(stderr, "Stitching failed.\n");
        goto fail;
    }

    *out_result = stitched;
    *frames_captured = frames.count;
    free(crops);
    sc_frame_list_clear(&frames);
    return 1;

fail:
    if (previous && frames.count == 0) {
        sc_image_free(previous);
    }
    sc_image_free(current);
    free(crops);
    sc_frame_list_clear(&frames);
    return 0;
}
