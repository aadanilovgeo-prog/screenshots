#include "updated_screenshoter.h"

#include <stdio.h>
#include <stdlib.h>

int us_run_capture_session(
    const UsRegion *region,
    const UsConfig *cfg,
    UsFrameList *frames,
    int *overlaps,
    int *fallbacks,
    int *overlap_count,
    int *reached_end
) {
    UsPageMetrics metrics;
    UsImage *previous = NULL;
    UsImage *current = NULL;
    int index;
    int cumulative_scroll = 0;

    us_get_page_metrics(region, &metrics);
    metrics.scroll_step = us_scroll_step_for_height(region->height, cfg->scroll_fraction);
    metrics.expected_overlap = us_expected_overlap_for_height(region->height, cfg->scroll_fraction);

    printf("Page metrics: viewport %dx%d, scrollStep=%d, expectedOverlap=%d\n",
           metrics.viewport_width,
           metrics.viewport_height,
           metrics.scroll_step,
           metrics.expected_overlap);

    us_focus_region(region);
    us_sleep_ms(250);

    previous = us_image_create(region->width, region->height);
    if (!previous || !us_capture_viewport(region, previous)) {
        us_image_free(previous);
        return 0;
    }

    if (!us_frame_list_push(frames, previous)) {
        us_image_free(previous);
        return 0;
    }

    printf("Captured frame 0000\n");

    for (index = 1; index <= cfg->max_frames; index++) {
        double diff;
        UsOverlapResult ov;
        UsImage *before_scroll;

        before_scroll = us_image_copy(previous);
        if (!before_scroll) {
            return 0;
        }

        us_scroll_to_next_position(region, &metrics);
        us_wait_for_page_stable(region, cfg->stable_wait_ms, 2500);
        us_sleep_ms(cfg->capture_delay_ms);

        current = us_image_create(region->width, region->height);
        if (!current || !us_capture_viewport(region, current)) {
            us_image_free(before_scroll);
            us_image_free(current);
            return 0;
        }

        diff = us_image_diff_ratio(before_scroll, current);
        us_image_free(before_scroll);

        if (diff < 0.002) {
            printf("Scroll position unchanged - end of page at frame %04d\n", index);
            *reached_end = 1;
            us_image_free(current);
            break;
        }

        ov = us_find_overlap(previous, current, &metrics);
        overlaps[*overlap_count] = ov.overlap;
        fallbacks[*overlap_count] = ov.used_fallback;
        (*overlap_count)++;

        cumulative_scroll += metrics.scroll_step;
        metrics.scroll_y = cumulative_scroll;
        metrics.document_scroll_height = cumulative_scroll + region->height;

        printf(
            "  Frame %04d: diff=%.2f%% overlap=%dpx confidence=%.2f%s\n",
            index,
            diff * 100.0,
            ov.overlap,
            ov.confidence,
            ov.used_fallback ? " [fallback]" : ""
        );

        if (!us_frame_list_push(frames, current)) {
            us_image_free(current);
            return 0;
        }

        previous = current;

        if (diff < 0.008 && ov.confidence < 0.4) {
            printf("Likely reached bottom of content.\n");
            *reached_end = 1;
            break;
        }

        if (*overlap_count >= cfg->max_frames) {
            break;
        }
    }

    us_stabilize_overlaps(overlaps, fallbacks, *overlap_count);
    return 1;
}
