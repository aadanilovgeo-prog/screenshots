#include "scroll_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int capture_when_stable(const ScRegion *region, ScImage *out) {
    ScImage *a;
    ScImage *b;
    size_t bytes;
    int attempt;

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

    for (attempt = 0; attempt < 6; attempt++) {
        sc_sleep_ms(120);
        if (!sc_capture_region(region, a)) {
            break;
        }
        if (sc_image_diff_ratio(a, b) < 0.004) {
            memcpy(out->rgb, a->rgb, bytes);
            sc_image_free(a);
            sc_image_free(b);
            return 1;
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
        printf("Focusing article region (mouse click)...\n");
        sc_focus_region(region);
        sc_sleep_ms(200);
    }

    previous = sc_image_create(region->width, region->height);
    if (!previous) {
        return 0;
    }
    if (!capture_when_stable(region, previous)) {
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

    printf("Capturing first frame...\n");

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
        if (!capture_when_stable(region, current)) {
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
            "  Frame %04d: diff %.2f%%, overlap %dpx\n",
            index,
            diff * 100.0,
            match.overlap
        );

        if (diff <= same_frame_threshold) {
            printf("End of page reached (frames are identical).\n");
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
