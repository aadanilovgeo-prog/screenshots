#include "updated_screenshoter.h"

void us_get_page_metrics(const UsRegion *region, UsPageMetrics *m) {
    if (!region || !m) {
        return;
    }

    m->viewport_width = region->width;
    m->viewport_height = region->height;
    m->device_pixel_ratio = 1.0;
    m->scroll_y = 0;
    m->document_scroll_height = region->height;
    m->capture_left = region->left;
    m->capture_top = region->top;
    m->scroll_step = us_scroll_step_for_height(region->height, US_SCROLL_FRACTION);
    m->expected_overlap = us_expected_overlap_for_height(region->height, US_SCROLL_FRACTION);
}

int us_scroll_step_for_height(int capture_height, double fraction) {
    int step;
    if (capture_height <= 0) {
        return 0;
    }
    if (fraction < 0.50) {
        fraction = 0.50;
    }
    if (fraction > 0.90) {
        fraction = 0.90;
    }
    step = (int)(capture_height * fraction);
    if (step < 40) {
        step = 40;
    }
    return step;
}

int us_expected_overlap_for_height(int capture_height, double fraction) {
    int overlap = capture_height - us_scroll_step_for_height(capture_height, fraction);
    if (overlap < 30) {
        overlap = 30;
    }
    return overlap;
}
