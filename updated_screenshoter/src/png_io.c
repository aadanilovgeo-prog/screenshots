#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#include "updated_screenshoter.h"

#include <stdio.h>

int us_save_png(const char *path, const UsImage *img) {
    if (!path || !img || !img->rgb) {
        return 0;
    }
    if (!stbi_write_png(path, img->width, img->height, 3, img->rgb, img->width * 3)) {
        fprintf(stderr, "Failed to save PNG: %s\n", path);
        return 0;
    }
    return 1;
}
