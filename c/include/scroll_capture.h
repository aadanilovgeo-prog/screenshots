#ifndef SCROLL_CAPTURE_H
#define SCROLL_CAPTURE_H

#include <stddef.h>

#define SC_WHEEL_DELTA 120
#define SC_PX_PER_WHEEL_NOTCH 35
#define SC_MAX_FRAMES 300

typedef struct {
    int left;
    int top;
    int width;
    int height;
} ScRegion;

typedef struct {
    int width;
    int height;
    unsigned char *rgb;
} ScImage;

typedef struct {
    ScImage **items;
    int count;
    int capacity;
} ScFrameList;

typedef struct {
    int overlap;
    double score;
} ScOverlapMatch;

typedef struct {
    int wheel_notches;
    int micro_steps;
    double micro_delay;
    int focus_click;
    int focus_each_step;
} ScScrollSettings;

typedef struct {
    int has_region;
    ScRegion region;
    char output_path[512];
    int has_output;
    int countdown;
    int wheel_notches;
    int micro_steps;
    double micro_delay;
    int no_focus_click;
    int focus_each_step;
    double scroll_delay;
    double settle_delay;
    int max_frames;
    double same_frame_threshold;
    int expected_overlap;
    char save_frames_dir[512];
    int has_save_frames;
} ScConfig;

int sc_config_parse(ScConfig *cfg, int argc, char **argv);
void sc_config_print_help(const char *prog);

ScImage *sc_image_create(int width, int height);
void sc_image_free(ScImage *img);
ScImage *sc_image_copy(const ScImage *src);
double sc_image_diff_ratio(const ScImage *a, const ScImage *b);

void sc_frame_list_init(ScFrameList *list);
int sc_frame_list_push(ScFrameList *list, ScImage *img);
void sc_frame_list_clear(ScFrameList *list);

int sc_overlap_search_bounds(
    int frame_height,
    int wheel_notches,
    int expected_overlap,
    int preferred_overlap,
    int has_preferred,
    int *min_overlap,
    int *max_overlap
);

ScOverlapMatch sc_find_vertical_overlap(
    const ScImage *above,
    const ScImage *below,
    int min_overlap,
    int max_overlap,
    int preferred_overlap,
    int has_preferred
);

int sc_stabilize_overlaps(int *overlaps, int count);
ScImage *sc_stitch_frames(const ScFrameList *frames, const int *overlaps, int overlap_count);

int sc_pick_region_interactive(ScRegion *region);
void sc_countdown(int seconds, const char *message);

int sc_focus_region(const ScRegion *region);
void sc_scroll_wheel_at(const ScRegion *region, const ScScrollSettings *scroll);

int sc_capture_region(const ScRegion *region, ScImage *out);
int sc_save_png(const char *path, const ScImage *img);
int sc_mkdir_p(const char *path);
void sc_sleep_ms(int milliseconds);
void sc_get_cursor_pos(int *x, int *y);
int sc_make_default_output_path(char *buf, size_t buflen);

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
);

#endif
