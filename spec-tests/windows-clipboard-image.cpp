#include "../lzc-mesh-client/windows/src/app.h"
#include "../lzc-mesh-client/windows/src/clipboard.h"
#include "../lzc-mesh-client/windows/src/files.h"
#include "../lzc-mesh-client/windows/src/net.h"

#include <ole2.h>
#include <shlobj.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>

App g;

namespace {
std::function<void(files::ClipboardImageResponse)> pending;
const std::string imageId(43, 'a');
const files::ImageBytes image = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x81, 0x34, 0x18, 0x00, 0x00, 0x49, 0xc8, 0x09, 0xf7, 0xf9,
    0xab, 0xb6, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82,
};

void copyText(HWND source, const wchar_t *text) {
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    assert(data);
    void *memory = GlobalLock(data);
    assert(memory);
    memcpy(memory, text, bytes);
    GlobalUnlock(data);
    assert(OpenClipboard(source));
    assert(EmptyClipboard());
    assert(SetClipboardData(CF_UNICODETEXT, data));
    assert(CloseClipboard());
}

void finishDownload(HWND window) {
    assert(pending);
    auto complete = std::exchange(pending, {});
    complete({200, ERROR_SUCCESS, image});
    MSG message{};
    assert(PeekMessageW(&message, window, WM_APP_CLIPBOARD_IMAGE, WM_APP_CLIPBOARD_IMAGE, PM_REMOVE));
    clipboard::onImageResult(message.lParam);
}

void assertImage(HWND window) {
    assert(OpenClipboard(window));
    const auto png = GetClipboardData(RegisterClipboardFormatW(L"PNG"));
    assert(png && GlobalSize(png) >= image.size());
    const void *bytes = GlobalLock(png);
    assert(bytes && !memcmp(bytes, image.data(), image.size()));
    GlobalUnlock(png);
    const auto dib = GetClipboardData(CF_DIB);
    const auto *bitmap = dib ? static_cast<const BITMAPINFOHEADER *>(GlobalLock(dib)) : nullptr;
    assert(bitmap && bitmap->biSize == sizeof(*bitmap) && bitmap->biWidth == 2 && bitmap->biHeight == 2);
    assert(bitmap->biBitCount == 24 && bitmap->biCompression == BI_RGB && bitmap->biSizeImage == 16);
    GlobalUnlock(dib);
    assert(CloseClipboard());
}

void assertVirtualFile() {
    IDataObject *data = nullptr;
    assert(OleGetClipboard(&data) == S_OK && data);
    const auto descriptorFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    const auto contentsFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));

    IEnumFORMATETC *formats = nullptr;
    assert(data->EnumFormatEtc(DATADIR_GET, &formats) == S_OK);
    bool hasDescriptor = false, hasContents = false;
    FORMATETC format{};
    while (formats->Next(1, &format, nullptr) == S_OK) {
        if (format.cfFormat == descriptorFormat) hasDescriptor = (format.tymed & TYMED_HGLOBAL) != 0;
        if (format.cfFormat == contentsFormat) hasContents = (format.tymed & TYMED_ISTREAM) != 0;
        CoTaskMemFree(format.ptd);
    }
    formats->Release();
    assert(hasDescriptor && hasContents);

    FORMATETC descriptor{descriptorFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    assert(data->GetData(&descriptor, &medium) == S_OK && medium.tymed == TYMED_HGLOBAL);
    assert(GlobalSize(medium.hGlobal) >= sizeof(FILEGROUPDESCRIPTORW));
    const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
    assert(group && group->cItems == 1);
    const auto &file = group->fgd[0];
    const std::wstring name(file.cFileName);
    assert(name.rfind(L"Screenshot-", 0) == 0 && name.substr(name.size() - 4) == L".png");
    assert((file.dwFlags & FD_FILESIZE) && file.nFileSizeHigh == 0 && file.nFileSizeLow == image.size());
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);

    FORMATETC contents{contentsFormat, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM};
    assert(data->GetData(&contents, &medium) == S_OK && medium.tymed == TYMED_ISTREAM);
    ULARGE_INTEGER position{};
    assert(medium.pstm->Seek({}, STREAM_SEEK_CUR, &position) == S_OK && position.QuadPart == image.size());
    assert(medium.pstm->Seek({}, STREAM_SEEK_SET, nullptr) == S_OK);
    files::ImageBytes saved(image.size());
    ULONG read = 0;
    assert(medium.pstm->Read(saved.data(), static_cast<ULONG>(saved.size()), &read) == S_OK);
    assert(read == saved.size() && saved == image);
    ReleaseStgMedium(&medium);

    contents.tymed = TYMED_HGLOBAL;
    assert(data->GetData(&contents, &medium) == S_OK && medium.tymed == TYMED_HGLOBAL);
    const auto *bytes = GlobalLock(medium.hGlobal);
    assert(bytes && GlobalSize(medium.hGlobal) >= image.size() && !memcmp(bytes, image.data(), image.size()));
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    data->Release();
}
}

std::wstring widen(const std::string &text) { return {text.begin(), text.end()}; }
std::string narrow(const std::wstring &text) { return {text.begin(), text.end()}; }
std::wstring fmt(const wchar_t *, ...) { return {}; }
void logLine(const std::wstring &) {}
void hubws::send(const std::string &) { assert(false); }
void files::cancelClipboardRequests() { pending = {}; }
void files::uploadClipboardImage(std::shared_ptr<const ImageBytes>) { assert(false); }
void files::requestClipboardImage(std::shared_ptr<const ImageBytes> png, const std::string &id, size_t size,
                                  std::function<void(ClipboardImageResponse)> complete) {
    assert(!png && id == imageId && size == image.size());
    pending = std::move(complete);
}

int main() {
    const HWND receiver = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    const HWND source = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    assert(receiver && source);
    g.hubConnected = g.deviceReady = g.deviceTrusted = g.authVerified = true;
    g.clipboardEnabled = g.clipboardImage = true;
    clipboard::start(receiver);
    clipboard::setActive(true);

    copyText(source, L"Copied before the image offer");
    clipboard::receiveImage(imageId, image.size(), "test");
    clipboard::onUpdate();
    finishDownload(receiver);
    assertImage(receiver);
    assertVirtualFile();
    clipboard::onUpdate();

    clipboard::set("repeated\ntext", "test");
    clipboard::receiveImage(imageId, image.size(), "test");
    clipboard::set("repeated\ntext", "test");
    finishDownload(receiver);
    assertImage(receiver);
    assertVirtualFile();

    clipboard::receiveImage(imageId, image.size(), "test");
    copyText(source, L"Copied after the image offer");
    finishDownload(receiver);
    assert(GetClipboardOwner() == source);
    assert(IsClipboardFormatAvailable(CF_UNICODETEXT));
    assert(!IsClipboardFormatAvailable(CF_DIB));

    clipboard::setActive(false);
    clipboard::stop();
    DestroyWindow(source);
    DestroyWindow(receiver);
    puts("Windows clipboard image ordering and formats passed");
}
