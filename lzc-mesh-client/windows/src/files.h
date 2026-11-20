#pragma once
#include "app.h"

#include <shellapi.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Shared files panel (Win32 list view + transfers on worker threads).
namespace files {
HWND create(HWND parent, HFONT font);
void setFont(HFONT font);
void layout(int x, int y, int w, int h);
const std::wstring &status();   // last transfer message, painted by the window
bool empty();
void update(const std::vector<SharedFile> &files);
void setStatusText(const std::wstring &text);
void setActive(bool on);
bool accepts(uint64_t ticket);
void pickAndUpload();
void onDrop(HDROP drop);
void downloadSelected();
void deleteSelected();
using ImageBytes = std::vector<uint8_t>;
struct ClipboardImageResponse {
    DWORD status = 0;
    DWORD error = ERROR_SUCCESS;
    ImageBytes body;
};
void cancelClipboardRequests();
void requestClipboardImage(std::shared_ptr<const ImageBytes> png, const std::string &id, size_t size,
                           std::function<void(ClipboardImageResponse)> complete);
void uploadClipboardImage(std::shared_ptr<const ImageBytes> png);
}
