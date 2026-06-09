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
        "Options:\n"
        "  --region L,T,W,H         Capture region\n"
        "  -o, --output FILE        Output PNG path\n"
        "  --countdown N            Seconds before start (default 5)\n"
        "  --wheel-notches N        Wheel notches per step (default 8)\n"
        "  --micro-steps N          Micro-scroll steps per step (default 8)\n"
        "  --micro-delay SEC        Delay between micro-scrolls (0.04)\n"
        "  --no-focus-click         Skip initial focus click\n"
        "  --focus-each-step        Click before each scroll step\n"
        "  --scroll-delay SEC       Delay after scroll (0.8)\n"
        "  --settle-delay SEC       Delay after screenshot (0.15)\n"
        "  --max-frames N           Frame limit (300)\n"
        "  --same-frame-threshold X End-of-page threshold (0.002)\n"
        "  --expected-overlap N     Expected overlap px (0=auto)\n"
        "  --save-frames DIR        Save debug frames to folder\n"
        "  -h, --help               Show help\n",
        prog
    );
}

int sc_config_parse(ScConfig *cfg, int argc, char **argv) {
    int i;

    if (!cfg) {
        return 0;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->countdown = 5;
    cfg->wheel_notches = 8;
    cfg->micro_steps = 8;
    cfg->micro_delay = 0.04;
    cfg->scroll_delay = 0.8;
    cfg->settle_delay = 0.15;
    cfg->max_frames = SC_MAX_FRAMES;
    cfg->same_frame_threshold = 0.002;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            sc_config_print_help(argv[0]);
            return 0;
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
        } else if (strcmp(arg, "--wheel-notches") == 0 || strcmp(arg, "--scroll-clicks") == 0) {
            if (i + 1 < argc) {
                cfg->wheel_notches = atoi(argv[++i]);
            }
        } else if (strcmp(arg, "--micro-steps") == 0 && i + 1 < argc) {
            cfg->micro_steps = atoi(argv[++i]);
        } else if (strcmp(arg, "--micro-delay") == 0 && i + 1 < argc) {
            if (!parse_double(argv[++i], &cfg->micro_delay)) {
                return 0;
            }
        } else if (strcmp(arg, "--no-focus-click") == 0) {
            cfg->no_focus_click = 1;
        } else if (strcmp(arg, "--focus-each-step") == 0) {
            cfg->focus_each_step = 1;
        } else if (strcmp(arg, "--scroll-delay") == 0 && i + 1 < argc) {
            if (!parse_double(argv[++i], &cfg->scroll_delay)) {
                return 0;
            }
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
        } else if (strcmp(arg, "--expected-overlap") == 0 && i + 1 < argc) {
            cfg->expected_overlap = atoi(argv[++i]);
        } else if (strcmp(arg, "--save-frames") == 0 && i + 1 < argc) {
            strncpy(cfg->save_frames_dir, argv[++i], sizeof(cfg->save_frames_dir) - 1);
            cfg->has_save_frames = 1;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", arg);
            return 0;
        }
    }

    return 1;
}
