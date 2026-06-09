#include "scroll_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sc_capture_long_page(
    const ScRegion *region,
    const ScScrollSettings *scroll,
    double scroll_delay,
    double settle_delay,
    int max_frames,
    double same_frame_threshold,
    int expected_overlap,
    const char *save_frames_dir,
    ScFrameList *frames,
    int *overlaps,
    int *overlap_count,
    int *reached_end
) {
    ScImage *previous = NULL;
    ScImage *current = NULL;
    int index;
    int has_preferred = expected_overlap > 0 ? 1 : 0;
    int preferred_overlap = expected_overlap > 0 ? expected_overlap : 0;
    int overlap_capacity = max_frames;
    char frame_path[600];

    if (!region || !scroll || !frames || !overlaps || !overlap_count || !reached_end) {
        return 0;
    }

    *overlap_count = 0;
    *reached_end = 0;

    if (scroll->focus_click) {
        printf("Фокус на области статьи (клик мышью)...\n");
        sc_focus_region(region);
        sc_sleep_ms(200);
    }

    previous = sc_image_create(region->width, region->height);
    if (!previous) {
        return 0;
    }
    if (!sc_capture_region(region, previous)) {
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

    printf("Захват первого кадра...\n");

    for (index = 1; index <= max_frames; index++) {
        ScOverlapMatch match;
        int min_overlap;
        int max_overlap;
        double diff;

        sc_scroll_wheel_at(region, scroll);
        sc_sleep_ms((int)(scroll_delay * 1000.0));

        current = sc_image_create(region->width, region->height);
        if (!current) {
            return 0;
        }
        if (!sc_capture_region(region, current)) {
            sc_image_free(current);
            return 0;
        }
        sc_sleep_ms((int)(settle_delay * 1000.0));

        diff = sc_image_diff_ratio(previous, current);
        sc_overlap_search_bounds(
            region->height,
            scroll->wheel_notches,
            expected_overlap,
            preferred_overlap,
            has_preferred,
            &min_overlap,
            &max_overlap
        );
        match = sc_find_vertical_overlap(
            previous,
            current,
            min_overlap,
            max_overlap,
            preferred_overlap,
            has_preferred
        );
        preferred_overlap = match.overlap;
        has_preferred = 1;

        overlaps[*overlap_count] = match.overlap;
        (*overlap_count)++;

        printf(
            "  Кадр %04d: отличие %.2f%%, перекрытие %dpx\n",
            index,
            diff * 100.0,
            match.overlap
        );

        if (diff <= same_frame_threshold) {
            printf("Страница, похоже, достигла конца (кадры совпадают).\n");
            *reached_end = 1;
            (*overlap_count)--;
            sc_image_free(current);
            break;
        }

        if (!sc_frame_list_push(frames, current)) {
            sc_image_free(current);
            return 0;
        }

        if (save_frames_dir) {
            snprintf(frame_path, sizeof(frame_path), "%s/frame_%04d.png", save_frames_dir, index);
            sc_save_png(frame_path, current);
        }

        previous = current;

        if (*overlap_count >= overlap_capacity) {
            break;
        }
    }

    sc_stabilize_overlaps(overlaps, *overlap_count);
    return 1;
}
