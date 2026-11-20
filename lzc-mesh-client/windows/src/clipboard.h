#pragma once
#include <cstdint>
#include <string>

// Shared clipboard. Listens with AddClipboardFormatListener on the main
// window and writes content that arrives from other devices.
namespace clipboard {
void start(HWND hwnd);
void stop();
void setActive(bool on);        // remove listener while dormant, baseline on wake
void onUpdate();                // WM_CLIPBOARDUPDATE
void set(const std::string &utf8, const std::string &from);
void interruptImage();
void receiveImage(const std::string &id, double size, const std::string &from);
void onImageResult(intptr_t value);
}
