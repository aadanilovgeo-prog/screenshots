#include "scroll_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_STABLE_ATTEMPTS 10
#define SC_STABLE_INTERVAL_MS 150
#define SC_STABLE_DIFF 0.0035
#define SC_MIN_ACCEPT_NEW_FRAC 0.08
#define SC_END_CONFIRM_ATTEMPTS 4
#define SC_END_RETRY_SETTLE_MS 450
#define SC_STALL_REPEAT_LIMIT 1
#define SC_STALL_DUP_RISK 0.82
#define SC_STALL_SHIFT_TOLERANCE 12
#define SC_STALL_THIN_OVERLAP_FRAC 0.20

typedef struct {
    int last_shift;
    int last_overlap;
    int repeat_count;
} ScStallTracker;

static void stall_tracker_reset(ScStallTracker *tracker) {
    if (!tracker) {
        return;
    }
    memset(tracker, 0, sizeof(*tracker));
}

static int stall_tracker_should_stop(
    ScStallTracker *tracker,
    const ScShiftData *shift,
    const ScSafeCrop *crop,
    int frame_height
) {
    int shift_similar;
    int overlap_similar;
    int high_dup;
    int thin_overlap;
    int bottom_bounce;
    int rubber_band;

    if (!tracker || !shift || !crop) {
        return 0;
    }

    shift_similar = tracker->last_shift > 0
        && abs(shift->detected_shift - tracker->last_shift) <= SC_STALL_SHIFT_TOLERANCE;
    overlap_similar = tracker->last_overlap > 0
        && abs(shift->detected_overlap - tracker->last_overlap) <= SC_STALL_SHIFT_TOLERANCE;
    high_dup = crop->duplicate_risk >= SC_STALL_DUP_RISK;
    thin_overlap = shift->detected_overlap < (int)(frame_height * SC_STALL_THIN_OVERLAP_FRAC);
    bottom_bounce = shift->scroll_overshoot && thin_overlap && high_dup;
    rubber_band = shift_similar && overlap_similar && high_dup && thin_overlap;

    if (bottom_bounce || rubber_band) {
        tracker->repeat_count++;
    } else {
        tracker->repeat_count = 0;
    }

    tracker->last_shift = shift->detected_shift;
    tracker->last_overlap = shift->detected_overlap;

    return tracker->repeat_count >= SC_STALL_REPEAT_LIMIT;
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

#define SC_SCROLL_END 2

static int page_still_scrollable(
    const ScImage *previous,
    const ScImage *current,
    int height,
    double same_frame_threshold
) {
    ScShiftData probe;

    if (sc_image_diff_ratio(previous, current) > same_frame_threshold) {
        return 1;
    }

    if (!sc_detect_vertical_content_shift(previous, current, &probe)) {
        return 0;
    }

    return probe.detected_shift >= (int)(height * SC_MIN_ACCEPT_NEW_FRAC);
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
    int no_change_attempts = 0;

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
        sc_scroll_wheel_notches(region, scroll, 1);
        sc_sleep_ms((int)(scroll->micro_delay * 1000.0));

        if (!sc_wait_for_frame_stable(region, current)) {
            return 0;
        }

        if (!page_still_scrollable(previous, current, height, same_frame_threshold)) {
            no_change_attempts++;
            printf(
                "  [info] No scroll movement detected (%d/%d), retrying...\n",
                no_change_attempts,
                SC_END_CONFIRM_ATTEMPTS
            );
            if (no_change_attempts == 2) {
                sc_focus_region(region);
                sc_sleep_ms(200);
            }
            if (no_change_attempts < SC_END_CONFIRM_ATTEMPTS) {
                sc_sleep_ms(SC_END_RETRY_SETTLE_MS + no_change_attempts * 150);
                continue;
            }
            return SC_SCROLL_END;
        }

        no_change_attempts = 0;

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

static int stitch_append_frame(
    ScImage **stitched,
    const ScImage *frame,
    int crop,
    int frame_index
) {
    ScImage *next;
    int new_h;

    if (!stitched || !*stitched || !frame) {
        return 0;
    }

    new_h = (*stitched)->height - crop + frame->height;
    if (new_h < frame->height) {
        new_h = frame->height;
    }
    if (!sc_image_size_ok(frame->width, new_h)) {
        int max_h = sc_image_max_height(frame->width);
        fprintf(
            stderr,
            "Output image would exceed memory limit at frame %d (height ~%d px, max ~%d px for width %d).\n",
            frame_index,
            new_h,
            max_h,
            frame->width
        );
        fprintf(
            stderr,
            "Saving partial result (%d px). Use a narrower capture region for longer pages.\n",
            (*stitched)->height
        );
        return 0;
    }

    next = sc_append_frame_safely(*stitched, frame, crop);
    if (!next) {
        fprintf(stderr, "Out of memory stitching frame %d.\n", frame_index);
        return 0;
    }

    sc_image_free(*stitched);
    *stitched = next;
    return 1;
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
    int index;
    int captured = 0;
    int max_output_height;
    ScStallTracker stall_tracker;
    char frame_path[600];
    char preview_path[600];

    if (!region || !scroll || !out_result || !frames_captured || !reached_end || !memory_limit_hit) {
        return 0;
    }

    *out_result = NULL;
    *frames_captured = 0;
    *reached_end = 0;
    *memory_limit_hit = 0;
    stall_tracker_reset(&stall_tracker);

    max_output_height = sc_image_max_height(region->width);

    if (scroll->focus_click) {
        printf("Focusing article region (mouse click)...\n");
        sc_focus_region(region);
        sc_sleep_ms(250);
    }

    previous = sc_image_create(region->width, region->height);
    if (!previous) {
        fprintf(stderr, "Out of memory for first frame.\n");
        return 0;
    }
    if (!sc_wait_for_frame_stable(region, previous)) {
        sc_image_free(previous);
        return 0;
    }

    stitched = sc_image_copy(previous);
    if (!stitched) {
        fprintf(stderr, "Out of memory for stitched image.\n");
        sc_image_free(previous);
        return 0;
    }
    captured = 1;

    if (save_frames_dir) {
        snprintf(frame_path, sizeof(frame_path), "%s/frame_0000.png", save_frames_dir);
        sc_save_png(frame_path, previous);
    }

    printf(
        "Capturing (target %.0f-%.0f%% new/frame, safe stitch=%s)...\n"
        "Max output height for this region: ~%d px (~%.0f MB)\n",
        scroll->min_new_frac * 100.0,
        scroll->max_new_frac * 100.0,
        safe_stitch ? "on" : "off",
        max_output_height,
        (double)SC_MAX_IMAGE_BYTES / (1024.0 * 1024.0)
    );

    for (index = 1; index <= max_frames; index++) {
        ScShiftData shift;
        ScSafeCrop safe_crop;
        int scroll_result;

        current = sc_image_create(region->width, region->height);
        if (!current) {
            fprintf(stderr, "Out of memory for frame %d.\n", index);
            break;
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
            printf(
                "End of page reached after %d failed scroll attempts (no movement).\n",
                SC_END_CONFIRM_ATTEMPTS
            );
            *reached_end = 1;
            sc_image_free(current);
            break;
        }
        if (scroll_result == 0) {
            sc_image_free(current);
            break;
        }

        {
            int saved_micro = shift.micro_steps_used;
            int saved_overshoot = shift.scroll_overshoot;

            sc_sleep_ms((int)(settle_delay * 1000.0));
            if (!sc_wait_for_frame_stable(region, current)) {
                sc_image_free(current);
                break;
            }

            if (!sc_detect_vertical_content_shift(previous, current, &shift)) {
                fprintf(stderr, "Failed to detect content shift for frame %d.\n", index);
                sc_image_free(current);
                break;
            }

            shift.micro_steps_used = saved_micro;
            shift.new_content_frac = (double)shift.detected_shift / (double)region->height;
            if (saved_overshoot) {
                shift.scroll_overshoot = 1;
                shift.confidence *= 0.65;
            }
        }

        safe_crop = sc_choose_safe_crop(previous, current, &shift, safe_stitch);
        sc_stitch_log_frame(log, index, &shift, &safe_crop, 0);

        if (stall_tracker_should_stop(&stall_tracker, &shift, &safe_crop, region->height)) {
            printf(
                "End of page: repeated scroll pattern detected "
                "(shift=%dpx overlap=%dpx dup_risk=%.2f, %d similar frames).\n",
                shift.detected_shift,
                shift.detected_overlap,
                safe_crop.duplicate_risk,
                SC_STALL_REPEAT_LIMIT + 1
            );
            sc_image_free(current);
            *reached_end = 1;
            break;
        }

        if (!stitch_append_frame(&stitched, current, safe_crop.crop, index)) {
            sc_image_free(current);
            *memory_limit_hit = 1;
            *reached_end = 1;
            break;
        }

        if (save_frames_dir) {
            snprintf(frame_path, sizeof(frame_path), "%s/frame_%04d.png", save_frames_dir, index);
            sc_save_png(frame_path, current);
            snprintf(preview_path, sizeof(preview_path), "%s/seam_%04d_preview.png", save_frames_dir, index);
            sc_save_seam_preview(preview_path, previous, current, safe_crop.crop);
        }

        sc_image_free(previous);
        previous = current;
        current = NULL;
        captured++;

        printf("  Stitched height: %d px (%d frame(s))\n", stitched->height, captured);

        if (max_output_height > 0 && stitched->height > (max_output_height * 9) / 10) {
            printf(
                "  [warn] Approaching memory limit (%d / ~%d px)\n",
                stitched->height,
                max_output_height
            );
        }

        if (captured >= max_frames) {
            break;
        }
    }

    sc_image_free(previous);
    sc_image_free(current);

    if (!stitched || captured < 1) {
        sc_image_free(stitched);
        return 0;
    }

    *out_result = stitched;
    *frames_captured = captured;
    return 1;
}
