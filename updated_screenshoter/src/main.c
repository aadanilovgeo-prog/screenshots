#include "updated_screenshoter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
int main(void) {
    fprintf(stderr, "updated_screenshoter requires Windows.\n");
    return 1;
}
#else

int main(int argc, char **argv) {
    UsConfig cfg;
    UsRegion region;
    UsFrameList frames;
    int *overlaps = NULL;
    int *fallbacks = NULL;
    int overlap_count = 0;
    int reached_end = 0;
    char output[512];
    UsImage *stitched = NULL;
    UsImage *final_img = NULL;
    int content_height = 0;
    int i;

    if (!us_config_parse(&cfg, argc, argv)) {
        return 0;
    }

    if (cfg.has_region) {
        sscanf(cfg.region_str, "%d,%d,%d,%d", &region.left, &region.top, &region.width, &region.height);
    } else if (!us_pick_region_interactive(&region)) {
        return 1;
    }

    printf("updated_screenshoter v1.0\n");
    printf("Capture region: %d,%d %dx%d\n", region.left, region.top, region.width, region.height);
    printf("Adaptive scroll: %.0f%% of viewport height per step\n", cfg.scroll_fraction * 100.0);
    printf("Max frames: %d\n", cfg.max_frames);

    us_countdown(cfg.countdown_sec, "Switch to article window. Capture starts in:");

    overlaps = (int *)calloc((size_t)cfg.max_frames, sizeof(int));
    fallbacks = (int *)calloc((size_t)cfg.max_frames, sizeof(int));
    if (!overlaps || !fallbacks) {
        fprintf(stderr, "Out of memory.\n");
        free(overlaps);
        free(fallbacks);
        return 1;
    }

    us_frame_list_init(&frames);

    if (!us_run_capture_session(&region, &cfg, &frames, overlaps, fallbacks, &overlap_count, &reached_end)) {
        fprintf(stderr, "Capture session failed.\n");
        us_frame_list_clear(&frames);
        free(overlaps);
        free(fallbacks);
        return 1;
    }

    if (frames.count < 1) {
        fprintf(stderr, "No frames captured.\n");
        us_frame_list_clear(&frames);
        free(overlaps);
        free(fallbacks);
        return 1;
    }

    printf("Stitching %d frames (%d overlaps)...\n", frames.count, overlap_count);

    us_stabilize_overlaps(overlaps, fallbacks, overlap_count);
    stitched = us_stitch_images(&frames, overlaps, overlap_count);
    if (!stitched) {
        fprintf(stderr, "Stitching failed.\n");
        us_frame_list_clear(&frames);
        free(overlaps);
        free(fallbacks);
        return 1;
    }

    content_height = frames.items[0]->height;
    for (i = 1; i < frames.count; i++) {
        int ov = overlaps[i - 1];
        if (ov < 1) {
            ov = 1;
        }
          content_height += frames.items[i]->height - ov;
    }

    final_img = us_finalize_long_screenshot(stitched, content_height);

    if (cfg.has_output) {
        strncpy(output, cfg.output_path, sizeof(output) - 1);
    } else {
        us_make_output_path(output, sizeof(output), cfg.save_to_downloads);
    }
    output[sizeof(output) - 1] = '\0';

    if (!us_save_png(output, final_img)) {
        us_image_free(final_img);
        us_frame_list_clear(&frames);
        free(overlaps);
        free(fallbacks);
        return 1;
    }

    printf("Done: %s\n", output);
    printf("Output size: %d x %d px\n", final_img->width, final_img->height);
    if (!reached_end) {
        printf("Warning: frame limit reached before end. Use --max-frames N\n");
    }

    us_image_free(final_img);
    us_frame_list_clear(&frames);
    free(overlaps);
    free(fallbacks);
    return 0;
}

#endif
