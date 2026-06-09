#include "updated_screenshoter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void us_config_print_help(const char *prog) {
    printf(
        "updated_screenshoter - adaptive long page screenshot tool\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --region L,T,W,H      Capture region (pixels)\n"
        "  -o, --output FILE     Output PNG path\n"
        "  --countdown N         Seconds before start (default 5)\n"
        "  --max-frames N        Max frames (default 600)\n"
        "  --scroll-fraction F   Scroll step as fraction of height (default 0.65)\n"
        "  --stable-wait MS      Delay after scroll stabilizes (default 300)\n"
        "  --capture-delay MS    Extra delay before capture (default 200)\n"
        "  --downloads           Save to Downloads folder\n"
        "  -h, --help            Show help\n",
        prog
    );
}

int us_config_parse(UsConfig *cfg, int argc, char **argv) {
    int i;
    if (!cfg) {
        return 0;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->countdown_sec = 5;
    cfg->max_frames = US_MAX_FRAMES;
    cfg->scroll_fraction = US_SCROLL_FRACTION;
    cfg->stable_wait_ms = 300;
    cfg->capture_delay_ms = 200;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            us_config_print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--region") == 0 && i + 1 < argc) {
            int l, t, w, h;
            if (sscanf(argv[++i], "%d,%d,%d,%d", &l, &t, &w, &h) != 4 || w <= 0 || h <= 0) {
                fprintf(stderr, "Invalid --region\n");
                return 0;
            }
            snprintf(cfg->region_str, sizeof(cfg->region_str), "%d,%d,%d,%d", l, t, w, h);
            cfg->has_region = 1;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            strncpy(cfg->output_path, argv[++i], sizeof(cfg->output_path) - 1);
            cfg->has_output = 1;
        } else if (strcmp(argv[i], "--countdown") == 0 && i + 1 < argc) {
            cfg->countdown_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
            cfg->max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--scroll-fraction") == 0 && i + 1 < argc) {
            cfg->scroll_fraction = atof(argv[++i]);
        } else if (strcmp(argv[i], "--stable-wait") == 0 && i + 1 < argc) {
            cfg->stable_wait_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--capture-delay") == 0 && i + 1 < argc) {
            cfg->capture_delay_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--downloads") == 0) {
            cfg->save_to_downloads = 1;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 0;
        }
    }
    return 1;
}
