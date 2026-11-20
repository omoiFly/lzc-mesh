#pragma once
#include "files.h"

namespace clipboard {
files::ImageBytes encodeClipboardBitmap(HBITMAP bitmap);
HGLOBAL decodeClipboardBitmap(const files::ImageBytes &png);
HGLOBAL createLegacyClipboardBitmap(HGLOBAL dibV5);
}
