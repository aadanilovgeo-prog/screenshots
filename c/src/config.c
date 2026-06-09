#include "scroll_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_region(const char *value, ScRegion *region) {
    int left, top, width, height;
    if (sscanf(value, "%d,%d,%d,%d", &left, &top, &width, &height) != 4) {
        return 0;
    }
    if (width <= 0 || height <= 0) {
        return 0;
    }
    region->left = left;
    region->top = top;
    region->width = width;
    region->height = height;
    return 1;
}

static int parse_double(const char *value, double *out) {
    char *end = NULL;
    double v = strtod(value, &end);
    if (!end || end == value) {
        return 0;
    }
    *out = v;
    return 1;
}

void sc_config_print_help(const char *prog) {
    printf(
        "Usage: %s [options]\n\n"
        "Scroll is adaptive: 1 wheel notch at a time until new content reaches\n"
        "  default (height > %d): %.0f-%.0f%% of capture height\n"
        "  small screen (height <= %d, ~13\"): %.0f-%.0f%%\n\n"
        "Options:\n"
        "  --region L,T,W,H         Capture region\n"
        "  -o, --output FILE        Output PNG path\n"
        "  --countdown N            Seconds before start (default 5)\n"
        "  --micro-delay SEC        Delay between wheel notches (default %.2f)\n"
        "  --no-focus-click         Skip initial focus click\n"
        "  --focus-each-step        Click before each wheel notch\n"
        "  --settle-delay SEC       Delay after frame stabilizes (0.15)\n"
        "  --max-frames N           Frame limit (600)\n"
        "  --same-frame-threshold X End-of-page threshold (0.002)\n"
        "  --no-safe-stitch         Disable safe stitch (not recommended)\n"
        "  --save-frames DIR        Save frames, seam previews, stitch_log.txt\n"
        "  --version                Show version\n"
        "  -h, --help               Show help\n",
        prog,
        SC_SMALL_SCREEN_HEIGHT,
        SC_MIN_NEW_FRAC * 100.0,
        SC_MAX_NEW_FRAC * 100.0,
        SC_SMALL_SCREEN_HEIGHT,
        SC_MIN_NEW_FRAC_SMALL * 100.0,
        SC_MAX_NEW_FRAC_SMALL * 100.0,
        SC_MICRO_DELAY_SEC
    );
}

int sc_config_parse(ScConfig *cfg, int argc, char **argv) {
    int i;

    if (!cfg) {
        return 0;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->countdown = 5;
    cfg->micro_delay = SC_MICRO_DELAY_SEC;
    cfg->settle_delay = 0.15;
    cfg->max_frames = SC_MAX_FRAMES;
    cfg->same_frame_threshold = 0.002;
    cfg->safe_stitch = 1;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            sc_config_print_help(argv[0]);
            return 0;
        } else if (strcmp(arg, "--version") == 0) {
            return 2;
        } else if (strcmp(arg, "--region") == 0 && i + 1 < argc) {
            if (!parse_region(argv[++i], &cfg->region)) {
                fprintf(stderr, "Invalid --region format (use left,top,width,height)\n");
                return 0;
            }
            cfg->has_region = 1;
        } else if ((strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) && i + 1 < argc) {
            strncpy(cfg->output_path, argv[++i], sizeof(cfg->output_path) - 1);
            cfg->has_output = 1;
        } else if (strcmp(arg, "--countdown") == 0 && i + 1 < argc) {
            cfg->countdown = atoi(argv[++i]);
        } else if (strcmp(arg, "--micro-delay") == 0 && i + 1 < argc) {
            if (!parse_double(argv[++i], &cfg->micro_delay)) {
                return 0;
            }
        } else if (strcmp(arg, "--no-focus-click") == 0) {
            cfg->no_focus_click = 1;
        } else if (strcmp(arg, "--focus-each-step") == 0) {
            cfg->focus_each_step = 1;
        } else if (strcmp(arg, "--settle-delay") == 0 && i + 1 < argc) {
            if (!parse_double(argv[++i], &cfg->settle_delay)) {
                return 0;
            }
        } else if (strcmp(arg, "--max-frames") == 0 && i + 1 < argc) {
            cfg->max_frames = atoi(argv[++i]);
        } else if (strcmp(arg, "--same-frame-threshold") == 0 && i + 1 < argc) {
            if (!parse_double(argv[++i], &cfg->same_frame_threshold)) {
                return 0;
            }
        } else if (strcmp(arg, "--no-safe-stitch") == 0) {
            cfg->safe_stitch = 0;
        } else if (strcmp(arg, "--save-frames") == 0 && i + 1 < argc) {
            strncpy(cfg->save_frames_dir, argv[++i], sizeof(cfg->save_frames_dir) - 1);
            cfg->has_save_frames = 1;
        } else if (
            strcmp(arg, "--wheel-notches") == 0 ||
            strcmp(arg, "--scroll-clicks") == 0 ||
            strcmp(arg, "--micro-steps") == 0 ||
            strcmp(arg, "--scroll-delay") == 0
        ) {
            fprintf(stderr, "Option %s was removed. Scroll is now adaptive (see --help).\n", arg);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", arg);
            return 0;
        }
    }

    return 1;
}
