#include "scroll_capture.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    ScConfig cfg;
    ScRegion region;
    ScScrollSettings scroll;
    ScStitchLog stitch_log;
    ScImage *result;
    int frames_captured = 0;
    int reached_end = 0;
    int memory_limit_hit = 0;
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
        "Adaptive scroll: %.0f-%.0f%% new/frame\n",
        region.left,
        region.top,
        region.width,
        region.height,
        scroll.min_new_frac * 100.0,
        scroll.max_new_frac * 100.0
    );

    sc_countdown(cfg.countdown, "Switch to the VRM window with the article. Capture starts in:");

    if (!sc_capture_long_page(
            &region,
            &scroll,
            cfg.settle_delay,
            cfg.max_frames,
            cfg.same_frame_threshold,
            cfg.safe_stitch,
            cfg.has_save_frames ? cfg.save_frames_dir : NULL,
            &result,
            &frames_captured,
            &reached_end,
            &memory_limit_hit,
            &stitch_log
        )) {
        fprintf(stderr, "Capture failed.\n");
        sc_image_free(result);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    if (frames_captured < 1 || !result) {
        fprintf(stderr, "No frames captured.\n");
        sc_image_free(result);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    if (frames_captured < 2 && !reached_end) {
        fprintf(
            stderr,
            "Only one frame captured - scrolling did not work.\n"
            "Try: --focus-each-step --settle-delay 0.25\n"
        );
        sc_image_free(result);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    printf("Captured %d frame(s), final size %d x %d px\n", frames_captured, result->width, result->height);

    if (cfg.has_output) {
        strncpy(output_path, cfg.output_path, sizeof(output_path) - 1);
    } else {
        sc_make_default_output_path(output_path, sizeof(output_path));
    }
    output_path[sizeof(output_path) - 1] = '\0';

    if (!sc_save_png(output_path, result)) {
        sc_image_free(result);
        sc_stitch_log_close(&stitch_log);
        return 1;
    }

    printf("Done: %s\n", output_path);
    printf("Output image size: %d x %d px\n", result->width, result->height);
    if (memory_limit_hit) {
        printf(
            "Warning: memory limit reached — partial screenshot saved (%d px height).\n"
            "Use a narrower capture region for longer pages.\n",
            result->height
        );
    } else if (!reached_end) {
        printf("Warning: max frame limit reached before end of page. Increase --max-frames if needed.\n");
    }

    sc_image_free(result);
    sc_stitch_log_close(&stitch_log);
    return 0;
}
