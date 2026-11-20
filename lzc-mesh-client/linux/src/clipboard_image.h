#pragma once
#include <glib.h>

void clipboard_image_cancel(void);
void clipboard_image_copy(int fd);
void clipboard_image_download(const char *id, gint64 size, const char *from);
