#ifndef UPDATED_SCREENSHOTER_H
#define UPDATED_SCREENSHOTER_H

#include <stddef.h>

#define US_MAX_FRAMES 600
#define US_SCROLL_FRACTION 0.72
#define US_HEADER_EXCLUDE_FRAC 0.12
#define US_OVERLAP_SEARCH_FRAC 0.38
#define US_MIN_CONFIDENCE 0.52
#define US_WHEEL_DELTA 120

typedef struct {
    int left;
    int top;
    int width;
    int height;
} UsRegion;

typedef struct {
    int viewport_width;
    int viewport_height;
    double device_pixel_ratio;
    int scroll_y;
    int document_scroll_height;
    int scroll_step;
    int expected_overlap;
    int capture_left;
    int capture_top;
} UsPageMetrics;

typedef struct {
    int width;
    int height;
    unsigned char *rgb;
} UsImage;

typedef struct {
    UsImage **items;
    int count;
    int capacity;
} UsFrameList;

typedef struct {
    int overlap;
    double confidence;
    int used_fallback;
} UsOverlapResult;

typedef struct {
    int countdown_sec;
    int max_frames;
    double scroll_fraction;
    int stable_wait_ms;
    int capture_delay_ms;
    int save_to_downloads;
    char output_path[512];
    int has_output;
    char region_str[64];
    int has_region;
} UsConfig;

/* metrics */
void us_get_page_metrics(const UsRegion *region, UsPageMetrics *m);
int us_scroll_step_for_height(int capture_height, double fraction);
int us_expected_overlap_for_height(int capture_height, double fraction);

/* image */
UsImage *us_image_create(int width, int height);
void us_image_free(UsImage *img);
UsImage *us_image_copy(const UsImage *src);
double us_image_diff_ratio(const UsImage *a, const UsImage *b);
UsImage *us_crop_top(const UsImage *src, int crop_rows);

void us_frame_list_init(UsFrameList *list);
int us_frame_list_push(UsFrameList *list, UsImage *img);
void us_frame_list_clear(UsFrameList *list);

/* capture / input */
int us_capture_viewport(const UsRegion *region, UsImage *out);
int us_wait_for_page_stable(const UsRegion *region, int stable_ms, int max_wait_ms);
int us_scroll_to_next_position(const UsRegion *region, const UsPageMetrics *metrics);
int us_focus_region(const UsRegion *region);
void us_sleep_ms(int ms);
void us_get_cursor_pos(int *x, int *y);

/* stitch */
UsOverlapResult us_find_overlap(const UsImage *previous, const UsImage *current, const UsPageMetrics *metrics);
int us_stabilize_overlaps(int *overlaps, int *fallbacks, int count);
UsImage *us_stitch_images(const UsFrameList *frames, const int *overlaps, int overlap_count);
UsImage *us_finalize_long_screenshot(UsImage *stitched, int content_height);

/* workflow */
int us_capture_viewport_to_file(const UsRegion *region, const char *path);
int us_pick_region_interactive(UsRegion *region);
void us_countdown(int seconds, const char *message);
int us_save_png(const char *path, const UsImage *img);
int us_make_output_path(char *buf, size_t buflen, int to_downloads);
int us_config_parse(UsConfig *cfg, int argc, char **argv);
void us_config_print_help(const char *prog);

int us_run_capture_session(
    const UsRegion *region,
    const UsConfig *cfg,
    UsFrameList *frames,
    int *overlaps,
    int *fallbacks,
    int *overlap_count,
    int *reached_end
);

#endif
