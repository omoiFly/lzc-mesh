#include "app.h"
#include "clipboard.h"

#include <commctrl.h>
#include <ole2.h>
#include <thread>

#include "../../transport/clipboard_png.h"
#include "clipboard_codec.h"
#include "clipboard_file.h"
#include "files.h"
#include "net.h"

namespace clipboard {
namespace {
HWND owner;
HWND imageOwner;
bool oleReady = false;
std::wstring lastText;
const size_t kMaxBytes = 256 * 1024;
UINT fmtExclude, fmtCanHistory, fmtCanCloud, fmtPNG;
bool active = false;
DWORD sequence = 0;
uint64_t imageEpoch = 0;
bool confirming = false;

struct Result {
    enum Operation { Encoded, Uploaded, Downloaded } operation;
    uint64_t epoch;
    DWORD sequence, status = 0;
    std::string from, id;
    std::shared_ptr<files::ImageBytes> png;
    HGLOBAL dibV5 = nullptr, dib = nullptr;
    DWORD networkError = ERROR_SUCCESS;
    ~Result() {
        if (dibV5) GlobalFree(dibV5);
        if (dib) GlobalFree(dib);
    }
};

bool imageReady() { return active && g.operational() && g.clipboardEnabled && g.clipboardImage; }

bool ownsImageClipboard() { return imageOwner && GetClipboardOwner() == imageOwner; }

bool current(const Result &result) {
    return imageReady() && imageEpoch == result.epoch &&
        (GetClipboardSequenceNumber() == result.sequence ||
         (result.operation == Result::Downloaded && ownsImageClipboard()));
}

void postResult(HWND window, Result *result) {
    if (!PostMessageW(window, WM_APP_CLIPBOARD_IMAGE, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

void confirmFileShare(const Result &result) {
    if (confirming) return;
    confirming = true;
    TASKDIALOG_BUTTON buttons[] = {{IDYES, L"通过文件共享发送"}};
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof dialog;
    dialog.hwndParent = IsWindowVisible(owner) ? owner : nullptr;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle = L"设备接力";
    dialog.pszMainInstruction = L"图片超出同步限制";
    dialog.pszContent = L"图片剪贴板支持最高 8K、最大 32 MiB。可以将原分辨率图片上传到文件共享，由其他设备下载。";
    dialog.cButtons = 1;
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    int selected = 0;
    const HRESULT status = TaskDialogIndirect(&dialog, &selected, nullptr, nullptr);
    confirming = false;
    if (SUCCEEDED(status) && selected == IDYES && current(result)) files::uploadClipboardImage(result.png);
}

void upload(const Result &result) {
    const auto dimensions = result.png && result.png->size() >= 45
        ? mesh_clipboard_png_size(result.png->data(), result.png->size()) : 0;
    if (!dimensions) {
        logLine(L"无法将本机剪贴板图片编码为 PNG");
        return;
    }
    if (dimensions < 0 || result.png->size() > MESH_CLIPBOARD_IMAGE_LIMIT) {
        confirmFileShare(result);
        return;
    }
    files::requestClipboardImage(result.png, "", 0, [window = owner, epoch = result.epoch, count = result.sequence,
                                                     png = result.png](files::ClipboardImageResponse response) {
        auto *completion = new Result{Result::Uploaded, epoch, count, response.status, {},
                                      std::string(response.body.begin(), response.body.end()), std::move(png)};
        completion->networkError = response.error;
        postResult(window, completion);
    });
}

std::shared_ptr<files::ImageBytes> readPNG() {
    const HANDLE memory = GetClipboardData(fmtPNG);
    const auto *bytes = memory ? static_cast<const uint8_t *>(GlobalLock(memory)) : nullptr;
    if (!bytes) return {};
    const auto length = GlobalSize(memory);
    auto offset = size_t{8};
    std::shared_ptr<files::ImageBytes> result;
    if (mesh_clipboard_png_size(bytes, length)) {
        while (length - offset >= 12) {
            const auto *chunk = bytes + offset;
            const size_t size = uint32_t(chunk[0]) << 24 | uint32_t(chunk[1]) << 16 | uint32_t(chunk[2]) << 8 | chunk[3];
            if (size > length - offset - 12) break;
            offset += size + 12;
            if (size == 0 && !memcmp(chunk + 4, "IEND", 4)) {
                result = std::make_shared<files::ImageBytes>(bytes, bytes + offset);
                break;
            }
        }
    }
    GlobalUnlock(memory);
    return result;
}

// Password managers mark secrets with these formats (Windows clipboard history
// convention); such content must never leave the machine.
bool isSecret() {
    if (IsClipboardFormatAvailable(fmtExclude)) return true;
    for (UINT f : {fmtCanHistory, fmtCanCloud}) {
        if (!IsClipboardFormatAvailable(f)) continue;
        HANDLE h = GetClipboardData(f);
        if (DWORD *v = h ? (DWORD *)GlobalLock(h) : nullptr) {
            bool deny = *v == 0;
            GlobalUnlock(h);
            if (deny) return true;
        }
    }
    return false;
}

bool openClipboard() {
    // Another process may hold the clipboard briefly; retry a few times.
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(20);
    }
    return false;
}

bool publishFormat(UINT format, HGLOBAL &memory) {
    if (!memory || !SetClipboardData(format, memory)) return false;
    memory = nullptr;
    return true;
}
}  // namespace

void start(HWND hwnd) {
    owner = hwnd;
    oleReady = SUCCEEDED(OleInitialize(nullptr));
    fmtExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    fmtCanHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    fmtCanCloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    fmtPNG = RegisterClipboardFormatW(L"PNG");
}

void stop() {
    if (!oleReady) return;
    if (ownsImageClipboard()) OleFlushClipboard();
    OleUninitialize();
    oleReady = false;
    imageOwner = nullptr;
}

void setActive(bool on) {
    if (active == on) return;
    active = on;
    interruptImage();
    lastText.clear();
    sequence = GetClipboardSequenceNumber(); // Never publish a change that happened during dormancy.
    if (on) AddClipboardFormatListener(owner);
    else RemoveClipboardFormatListener(owner);
}

void onUpdate() {
    if (!active || !g.operational()) return;
    const auto current = GetClipboardSequenceNumber();
    if (current == sequence) return;
    sequence = current;
    if (GetClipboardOwner() == owner || ownsImageClipboard()) return;  // our own write
    interruptImage();
    if (!g.clipboardEnabled) return;
    const bool textAvailable = IsClipboardFormatAvailable(CF_UNICODETEXT);
    const bool imageAvailable = imageReady() && !IsClipboardFormatAvailable(CF_HDROP)
        && (IsClipboardFormatAvailable(fmtPNG) || IsClipboardFormatAvailable(CF_BITMAP));
    if ((!textAvailable && !imageAvailable) || !openClipboard()) return;
    std::wstring text;
    std::shared_ptr<files::ImageBytes> png;
    HBITMAP bitmap = nullptr;
    const bool secret = isSecret();
    if (!secret) {
        if (HANDLE h = textAvailable ? GetClipboardData(CF_UNICODETEXT) : nullptr) {
            if (const wchar_t *p = (const wchar_t *)GlobalLock(h)) {
                text = p;
                GlobalUnlock(h);
            }
        }
        if (imageAvailable) {
            if (IsClipboardFormatAvailable(fmtPNG)) png = readPNG();
            if (!png) {
                if (HANDLE image = GetClipboardData(CF_BITMAP))
                    bitmap = static_cast<HBITMAP>(CopyImage(image, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
            }
        }
    }
    sequence = GetClipboardSequenceNumber();
    CloseClipboard();
    if (!secret && !text.empty() && text != lastText) {
        std::string utf8 = narrow(text);
        if (utf8.size() > kMaxBytes) logLine(L"剪贴板文本超过 256 KB，未同步");
        else {
            lastText = text;
            hubws::send(JsonOut().s("t", "clipboard").s("text", utf8).str());
            logLine(fmt(L"已同步本机剪贴板（%zu 字节）", utf8.size()));
        }
    } else if (!textAvailable) {
        lastText.clear();
    }
    if (png) {
        Result result{Result::Encoded, imageEpoch, sequence, 0, {}, {}, std::move(png)};
        if (clipboard::current(result)) upload(result);
    } else if (bitmap) {
        std::thread([window = owner, epoch = imageEpoch, count = sequence, bitmap] {
            auto png = encodeClipboardBitmap(bitmap);
            DeleteObject(bitmap);
            auto *result = new Result{Result::Encoded, epoch, count, 0, {}, {},
                                      std::make_shared<files::ImageBytes>(std::move(png))};
            postResult(window, result);
        }).detach();
    } else if (!secret && imageAvailable) logLine(L"无法读取本机剪贴板图片");
}

void set(const std::string &utf8, const std::string &from) {
    if (!active || !g.operational() || !g.clipboardEnabled || utf8.empty()) return;
    std::wstring text = widen(utf8), crlf;
    // Windows wants CRLF line endings in CF_UNICODETEXT.
    for (wchar_t c : text) {
        if (c == L'\n' && (crlf.empty() || crlf.back() != L'\r')) crlf += L'\r';
        crlf += c;
    }
    if (crlf == lastText) return;
    interruptImage();
    if (!openClipboard()) {
        logLine(L"写入剪贴板失败：剪贴板被其他程序占用");
        return;
    }
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (crlf.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), crlf.c_str(), (crlf.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    sequence = GetClipboardSequenceNumber();
    CloseClipboard();
    lastText = crlf;
    logLine(fmt(L"已接收来自 %ls 的剪贴板（%zu 字节）", from.empty() ? L"其他设备" : widen(from).c_str(), utf8.size()));
}

void interruptImage() {
    ++imageEpoch;
    files::cancelClipboardRequests();
}

void receiveImage(const std::string &id, double size, const std::string &from) {
    if (!imageReady() || !(size >= 45 && size <= MESH_CLIPBOARD_IMAGE_LIMIT) ||
        size != static_cast<size_t>(size) || !mesh_clipboard_image_id(id.c_str())) return;
    sequence = GetClipboardSequenceNumber();
    interruptImage();
    logLine(fmt(L"正在接收剪贴板图片（%zu 字节）", static_cast<size_t>(size)));
    files::requestClipboardImage({}, id, static_cast<size_t>(size),
        [window = owner, epoch = imageEpoch, count = sequence, from](files::ClipboardImageResponse response) {
            auto *result = new Result{Result::Downloaded, epoch, count, response.status, from, {},
                                      std::make_shared<files::ImageBytes>(std::move(response.body))};
            result->networkError = response.error;
            if (response.status == 200 && !response.error) {
                result->dibV5 = decodeClipboardBitmap(*result->png);
                if (result->dibV5) result->dib = createLegacyClipboardBitmap(result->dibV5);
            }
            postResult(window, result);
        });
}

void onImageResult(intptr_t value) {
    std::unique_ptr<Result> result(reinterpret_cast<Result *>(value));
    if (!current(*result)) return;
    if (result->operation == Result::Encoded) { upload(*result); return; }
    if (result->networkError) {
        logLine(fmt(L"图片剪贴板%ls失败（HTTP %lu，Windows 错误 %lu）",
                    result->operation == Result::Uploaded ? L"上传" : L"下载", result->status, result->networkError));
        return;
    }
    if (result->operation == Result::Uploaded) {
        if (result->status == 201 && mesh_clipboard_image_id(result->id.c_str())) {
            hubws::send(JsonOut().s("t", "clipboard_image").s("id", result->id).str());
            logLine(fmt(L"已发送剪贴板图片（%zu 字节）", result->png->size()));
        } else if (result->status == 413 || result->status == 422) confirmFileShare(*result);
        else logLine(fmt(L"图片剪贴板上传失败（HTTP %lu）", result->status));
        return;
    }
    if (result->status != 200) {
        logLine(fmt(L"图片剪贴板下载失败（HTTP %lu）", result->status));
        return;
    }
    if (!result->dibV5) { logLine(L"无法解码收到的剪贴板图片"); return; }
    HRESULT published = CO_E_NOTINITIALIZED;
    if (oleReady) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (!current(*result)) return;
            published = setImageClipboard(fmtPNG, result->png, result->dibV5, result->dib);
            if (published != CLIPBRD_E_CANT_OPEN) break;
            Sleep(20);
        }
    }
    if (SUCCEEDED(published)) {
        const HWND newOwner = GetClipboardOwner();
        imageOwner = GetWindowThreadProcessId(newOwner, nullptr) == GetCurrentThreadId() ? newOwner : nullptr;
        sequence = GetClipboardSequenceNumber();
        lastText.clear();
        logLine(fmt(L"已接收来自 %ls 的剪贴板图片，可在资源管理器粘贴为 PNG 文件",
                    result->from.empty() ? L"其他设备" : widen(result->from).c_str()));
        return;
    }
    logLine(fmt(L"无法提供 PNG 文件粘贴（HRESULT 0x%08lX），将写入图片剪贴板",
                static_cast<unsigned long>(published)));
    HGLOBAL png = GlobalAlloc(GMEM_MOVEABLE, result->png->size());
    void *buffer = png ? GlobalLock(png) : nullptr;
    if (buffer) {
        memcpy(buffer, result->png->data(), result->png->size());
        GlobalUnlock(png);
    } else if (png) {
        GlobalFree(png);
        png = nullptr;
    }
    if (openClipboard()) {
        const bool stillCurrent = current(*result);
        bool setPNG = false, setDibV5 = false, setDib = false;
        if (stillCurrent && EmptyClipboard()) {
            setPNG = publishFormat(fmtPNG, png);
            setDibV5 = publishFormat(CF_DIBV5, result->dibV5);
            setDib = publishFormat(CF_DIB, result->dib);
            sequence = GetClipboardSequenceNumber();
        }
        CloseClipboard();
        if (stillCurrent) {
            if (setPNG || setDibV5 || setDib) {
                lastText.clear();
                logLine(fmt(L"已将来自 %ls 的图片写入剪贴板（PNG=%d，DIBV5=%d，DIB=%d）",
                            result->from.empty() ? L"其他设备" : widen(result->from).c_str(), setPNG, setDibV5, setDib));
            } else logLine(L"写入剪贴板图片失败");
        }
    } else logLine(L"写入剪贴板图片失败：剪贴板被其他程序占用");
    if (png) GlobalFree(png);
}
}  // namespace clipboard
