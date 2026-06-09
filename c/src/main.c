#include "scroll_capture.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    ScConfig cfg;
    ScRegion region;
    ScScrollSettings scroll;
    ScFrameList frames;
    ScStitchLog stitch_log;
    ScImage *result;
    int *crops;
    int crop_count = 0;
    int reached_end = 0;
    int parse_result;
    char output_path[512];

    parse_result = sc_config_parse(&cfg, argc, argv);
    if (parse_result == 0) {
        return 1;
    }
    if (parse_result == 2) {
        printf("scroll_capture v%s\n", SC_VERSION);
        return 0;
    }

    if (cfg.has_region) {
        region = cfg.region;
    } else if (!sc_pick_region_interactive(&region)) {
        return 1;
    }

    sc_scroll_settings_init(&region, &cfg, &scroll);

    if (cfg.has_save_frames) {
        sc_mkdir_p(cfg.save_frames_dir);
    }

    memset(&stitch_log, 0, sizeof(stitch_log));
    if (cfg.has_save_frames) {
        sc_stitch_log_init(&stitch_log, cfg.save_frames_dir);
    }

    printf(
        "Region: %d,%d %dx%d\n"
        "Adaptive scroll: %.0f-%.0f%% new/frame, %d wheel notch(es)/step (%s)\n",
        region.left,
        region.top,
        region.width,
        region.height,
        scroll.min_new_frac * 100.0,
        scroll.max_new_frac * 100.0,
        scroll.notches_per_step,
        region.height <= SC_SMALL_SCREEN_HEIGHT ? "small screen" : "large screen"
    );

    sc_countdown(cfg.countdown, "Switch to the VRM window with the article. Capture starts in:");

    crops = (int *)calloc((size_t)cfg.max_frames, sizeof(int));
    if (!crops) {
        fprintf(stderr, "Out of memory.\n");
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    sc_frame_list_init(&frames);

    if (!sc_capture_long_page(
            &region,
            &scroll,
            cfg.settle_delay,
            cfg.max_frames,
            cfg.same_frame_threshold,
            cfg.safe_stitch,
            cfg.has_save_frames ? cfg.save_frames_dir : NULL,
            &frames,
            crops,
            &crop_count,
            &reached_end,
            &stitch_log
        )) {
        fprintf(stderr, "Capture failed.\n");
        free(crops);
        sc_frame_list_clear(&frames);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    if (frames.count < 1) {
        fprintf(stderr, "No frames captured.\n");
        free(crops);
        sc_frame_list_clear(&frames);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    if (frames.count < 2 && !reached_end) {
        fprintf(
            stderr,
            "Only one frame captured - scrolling did not work.\n"
            "Try: --focus-each-step --settle-delay 0.25\n"
        );
        free(crops);
        sc_frame_list_clear(&frames);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    printf("Stitching %d frames (safe mode, per-seam crop)...\n", frames.count);
    result = sc_stitch_frames_safe(&frames, crops, crop_count);
    if (!result) {
        fprintf(stderr, "Stitching failed.\n");
        free(crops);
        sc_frame_list_clear(&frames);
        sc_stitch_log_close(&stitch_log);
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
        free(crops);
        sc_frame_list_clear(&frames);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    printf("Done: %s\n", output_path);
    printf("Output image size: %d x %d px\n", result->width, result->height);
    if (!reached_end) {
        printf("Warning: max frame limit reached before end of page. Increase --max-frames if needed.\n");
    }

    sc_image_free(result);
    free(crops);
    sc_frame_list_clear(&frames);
    sc_stitch_log_close(&stitch_log);
    return 0;
}
