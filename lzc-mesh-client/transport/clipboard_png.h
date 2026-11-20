#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MESH_CLIPBOARD_IMAGE_LIMIT (32u * 1024u * 1024u)

static inline int mesh_clipboard_png_size(const uint8_t *data, size_t length) {
    if (length < 24 || memcmp(data, "\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16)) return 0;
    const uint32_t width = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 |
                           (uint32_t)data[18] << 8 | data[19];
    const uint32_t height = (uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 |
                            (uint32_t)data[22] << 8 | data[23];
    if (!width || !height) return 0;
    return width <= 7680 && height <= 7680 && (width <= 4320 || height <= 4320) ? 1 : -1;
}

static inline int mesh_clipboard_image_id(const char *id) {
    return strlen(id) == 43 &&
        strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == 43;
}
