#ifndef SCROLL_CAPTURE_H
#define SCROLL_CAPTURE_H

#include <stddef.h>
#include <stdio.h>

#define SC_WHEEL_DELTA 120
#define SC_MAX_FRAMES 600

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
    int safe_stitch;
    char save_frames_dir[512];
    int has_save_frames;
} ScConfig;

typedef struct {
    int detected_shift;
    int detected_overlap;
    int initial_crop;
    double confidence;
    double match_score;
} ScShiftData;

typedef struct {
    int crop;
    int initial_crop;
    double confidence;
    double content_loss_risk;
    double duplicate_risk;
    double table_seam_risk;
    double image_seam_risk;
    int weak_seam;
    int safe_mode;
} ScSafeCrop;

typedef struct {
    FILE *log_file;
    char debug_dir[512];
    int has_debug_dir;
    int frame_index;
} ScStitchLog;

int sc_config_parse(ScConfig *cfg, int argc, char **argv);
void sc_config_print_help(const char *prog);

ScImage *sc_image_create(int width, int height);
void sc_image_free(ScImage *img);
ScImage *sc_image_copy(const ScImage *src);
ScImage *sc_image_append_crop(const ScImage *result, const ScImage *frame, int top_crop);
double sc_image_diff_ratio(const ScImage *a, const ScImage *b);

void sc_frame_list_init(ScFrameList *list);
int sc_frame_list_push(ScFrameList *list, ScImage *img);
void sc_frame_list_clear(ScFrameList *list);

int sc_detect_vertical_content_shift(
    const ScImage *above,
    const ScImage *below,
    ScShiftData *out
);

ScSafeCrop sc_choose_safe_crop(
    const ScImage *above,
    const ScImage *below,
    const ScShiftData *shift,
    int safe_mode
);

double sc_evaluate_seam(const ScImage *above, const ScImage *below, int crop);

ScImage *sc_append_frame_safely(const ScImage *result, const ScImage *frame, int safe_crop);
ScImage *sc_stitch_frames_safe(const ScFrameList *frames, const int *crops, int crop_count);

void sc_stitch_log_init(ScStitchLog *log, const char *debug_dir);
void sc_stitch_log_close(ScStitchLog *log);
void sc_stitch_log_frame(
    ScStitchLog *log,
    int frame_index,
    const ScShiftData *shift,
    const ScSafeCrop *crop,
    int end_of_page
);
int sc_save_seam_preview(
    const char *path,
    const ScImage *above,
    const ScImage *below,
    int crop
);

int sc_pick_region_interactive(ScRegion *region);
void sc_countdown(int seconds, const char *message);

int sc_focus_region(const ScRegion *region);
void sc_scroll_wheel_at(const ScRegion *region, const ScScrollSettings *scroll);

int sc_capture_region(const ScRegion *region, ScImage *out);
int sc_wait_for_frame_stable(const ScRegion *region, ScImage *out);
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
    int safe_stitch,
    const char *save_frames_dir,
    ScFrameList *frames,
    int *crops,
    int *crop_count,
    int *reached_end,
    ScStitchLog *log
);

#endif
