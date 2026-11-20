#pragma once

#include "files.h"

namespace clipboard {
HRESULT setImageClipboard(UINT pngFormat, const std::shared_ptr<files::ImageBytes> &png,
                          HGLOBAL &dibV5, HGLOBAL &dib);
}
