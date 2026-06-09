#include "scroll_capture.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_int_for_median(const void *a, const void *b) {
    return (*(const int *)a) - (*(const int *)b);
}

static int median_overlap(const int *values, int count) {
    int *copy;
    int result;

    if (count <= 0) {
        return 0;
    }

    copy = (int *)malloc((size_t)count * sizeof(int));
    if (!copy) {
        return values[count / 2];
    }

    memcpy(copy, values, (size_t)count * sizeof(int));
    qsort(copy, (size_t)count, sizeof(int), compare_int_for_median);
    result = copy[count / 2];
    free(copy);
    return result;
}

int main(int argc, char **argv) {
    ScConfig cfg;
    ScRegion region;
    ScScrollSettings scroll;
    ScFrameList frames;
    int *overlaps = NULL;
    int overlap_count = 0;
    int reached_end = 0;
    int expected_overlap;
    int estimated_scroll;
    char output_path[512];
    ScImage *result = NULL;
    int min_overlap = 0;
    int max_overlap = 0;
    int i;

#ifndef _WIN32
    fprintf(stderr, "Note: full functionality is available on Windows only.\n");
#endif

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("scroll_capture %s\n", SC_VERSION);
        return 0;
    }

    if (!sc_config_parse(&cfg, argc, argv)) {
        return cfg.countdown == 5 && cfg.wheel_notches == 10 ? 0 : 1;
    }

    if (cfg.has_region) {
        region = cfg.region;
    } else if (!sc_pick_region_interactive(&region)) {
        return 1;
    }

    scroll.wheel_notches = cfg.wheel_notches;
    scroll.micro_steps = cfg.micro_steps;
    scroll.micro_delay = cfg.micro_delay;
    scroll.focus_click = !cfg.no_focus_click;
    scroll.focus_each_step = cfg.focus_each_step;

    expected_overlap = cfg.expected_overlap;
    estimated_scroll = cfg.wheel_notches * SC_PX_PER_WHEEL_NOTCH;
    if (estimated_scroll < 40) {
        estimated_scroll = 40;
    }
    if (expected_overlap <= 0) {
        expected_overlap = region.height - estimated_scroll;
        if (expected_overlap < region.height / 3) {
            expected_overlap = region.height / 3;
        }
    }

    printf("scroll_capture v%s\n", SC_VERSION);
    printf(
        "Capture region: left=%d, top=%d, width=%d, height=%d\n",
        region.left,
        region.top,
        region.width,
        region.height
    );
    printf(
        "Scroll: wheel_notches=%d, micro_steps=%d, expected_overlap~%dpx\n",
        scroll.wheel_notches,
        scroll.micro_steps,
        expected_overlap
    );
    printf("Emergency stop: close console window or press Ctrl+C.\n");

    if (cfg.has_save_frames) {
        sc_mkdir_p(cfg.save_frames_dir);
    }

    sc_countdown(cfg.countdown, "Switch to the VRM/browser window with the article. Capture starts in:");

    overlaps = (int *)calloc((size_t)cfg.max_frames, sizeof(int));
    if (!overlaps) {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }

    sc_frame_list_init(&frames);

    if (!sc_capture_long_page(
            &region,
            &scroll,
            cfg.scroll_delay,
            cfg.settle_delay,
            cfg.max_frames,
            cfg.same_frame_threshold,
            expected_overlap,
            cfg.has_save_frames ? cfg.save_frames_dir : NULL,
            &frames,
            overlaps,
            &overlap_count,
            &reached_end
        )) {
        fprintf(stderr, "Capture failed.\n");
        free(overlaps);
        sc_frame_list_clear(&frames);
        return 1;
    }

    if (frames.count < 2 && !reached_end) {
        fprintf(
            stderr,
            "Only one frame captured - scrolling did not work.\n"
            "Try: --wheel-notches 12 --focus-each-step --scroll-delay 1.0\n"
        );
        free(overlaps);
        sc_frame_list_clear(&frames);
        return 1;
    }

    printf("Stitching %d frames...\n", frames.count);
    if (overlap_count > 0) {
        for (i = 0; i < overlap_count; i++) {
            if (i == 0) {
                min_overlap = max_overlap = overlaps[i];
            } else {
                if (overlaps[i] < min_overlap) {
                    min_overlap = overlaps[i];
                }
                if (overlaps[i] > max_overlap) {
                    max_overlap = overlaps[i];
                }
            }
        }
        printf(
            "  Overlaps after stabilization: min=%d, max=%d, median=%d px\n",
            min_overlap,
            max_overlap,
            median_overlap(overlaps, overlap_count)
        );
    }

    result = sc_stitch_frames(&frames, overlaps, overlap_count);
    if (!result) {
        fprintf(stderr, "Stitching failed.\n");
        free(overlaps);
        sc_frame_list_clear(&frames);
        return 1;
    }

    if (cfg.has_output) {
        strncpy(output_path, cfg.output_path, sizeof(output_path) - 1);
    } else {
        sc_make_default_output_path(output_path, sizeof(output_path));
    }
    output_path[sizeof(output_path) - 1] = '\0';

    if (!sc_save_png(output_path, result)) {
        sc_image_free(result);
        free(overlaps);
        sc_frame_list_clear(&frames);
        return 1;
    }

    printf("Done: %s\n", output_path);
    printf("Output image size: %d x %d px\n", result->width, result->height);
    if (!reached_end) {
        printf("Warning: max frame limit reached before end of page. Increase --max-frames if needed.\n");
    }

    sc_image_free(result);
    free(overlaps);
    sc_frame_list_clear(&frames);
    return 0;
}
