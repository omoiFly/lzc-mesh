// Shared files for the Windows client: a list view with upload / download /
// delete, upload via file dialog or drag and drop onto the window. Transfers
// run on worker threads with WinHTTP and stream from/to disk.
#include "app.h"
#include "files.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>

#include <thread>
#include <atomic>

#include "net.h"

namespace files {
namespace {
HWND list, parentWnd;
std::wstring statusText = L"所有设备都能下载；下载到“下载”文件夹，也可把文件拖到窗口上";
std::vector<SharedFile> current;
std::atomic<bool> active{false};
std::atomic<uint64_t> generation{0};
std::atomic<uint64_t> imageGeneration{1};
std::mutex activityMutex;
thread_local uint64_t workerGeneration = 0;
bool valid(uint64_t ticket) { return active && generation == ticket; }

std::wstring escape(const std::wstring &name) {
    std::string u = narrow(name), out;
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); out += b; }
    }
    return widen(out);
}

std::wstring sizeText(long long n) {
    wchar_t buf[64];
    StrFormatByteSizeW(n, buf, 64);
    return buf;
}

void postStatus(const std::wstring &text) {
    if (!valid(workerGeneration)) return;
    auto *value = new std::wstring(text);
    if (!PostMessageW(parentWnd, WM_APP_FILES_STATUS, (WPARAM)workerGeneration, (LPARAM)value)) delete value;
}

// The worker owns the synchronous WinHTTP handles. Cancellation invalidates
// its ticket so it can close the handles once the current call returns.
struct Req {
    HINTERNET s = nullptr, c = nullptr, r = nullptr;
    const uint64_t ticket, imageTicket;
    explicit Req(uint64_t epoch, uint64_t imageEpoch = 0): ticket(epoch), imageTicket(imageEpoch) {}
    Req(const Req &) = delete;
    Req &operator=(const Req &) = delete;
    bool current() const { return valid(ticket) && (!imageTicket || imageGeneration == imageTicket); }
    ~Req() {
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
    }
    bool open(const wchar_t *method, const std::wstring &host, const std::wstring &name, const std::wstring &cookie, const std::wstring &device, const std::wstring &token) {
        return openPath(method, host, L"/api/files/" + escape(name), cookie, device, token);
    }
    bool openPath(const wchar_t *method, const std::wstring &host, const std::wstring &path, const std::wstring &cookie, const std::wstring &device, const std::wstring &token) {
        if (!current()) return false;
        s = WinHttpOpen(L"lzc-mesh-windows/" LZC_MESH_VERSION, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) return false;
        WinHttpSetTimeouts(s, 15000, 15000, 120000, 120000);
        c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!c) return false;
        r = WinHttpOpenRequest(c, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!r || !current()) return false;
        DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof noRedirect);
        std::wstring h = L"Cookie: Lzc-Auth-Token=" + cookie + L"\r\nX-Lzc-Mesh-Device: " + escape(device) + L"\r\nX-Lzc-Mesh-Device-Id: " + widen(g.deviceId) + L"\r\nX-Lzc-Mesh-Device-Token: " + token + L"\r\n";
        return WinHttpAddRequestHeaders(r, h.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    bool send(DWORD size = 0) {
        return current() && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, size, 0);
    }
    bool receive() { return current() && WinHttpReceiveResponse(r, nullptr); }
    DWORD status() {
        DWORD code = 0, len = sizeof code;
        if (current())
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
        return code;
    }

    bool sendImage(const ImageBytes &bytes) {
        if (!current() || bytes.size() > MAXDWORD ||
            !WinHttpSendRequest(r, L"Content-Type: image/png\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, DWORD(bytes.size()), 0)) return false;
        for (size_t offset = 0; offset < bytes.size();) {
            const DWORD count = DWORD(std::min<size_t>(bytes.size() - offset, 64 * 1024));
            DWORD written = 0;
            if (!current() || !WinHttpWriteData(r, bytes.data() + offset, count, &written) || written != count) return false;
            offset += written;
        }
        return receive();
    }

    DWORD readImageResponse(ImageBytes &bytes, size_t size) {
        bytes.resize(size);
        for (size_t offset = 0; offset < size;) {
            const DWORD count = DWORD(std::min<size_t>(size - offset, 64 * 1024));
            DWORD read = 0;
            if (!current()) return ERROR_OPERATION_ABORTED;
            if (!WinHttpReadData(r, bytes.data() + offset, count, &read)) return GetLastError();
            if (!read) return ERROR_HANDLE_EOF;
            offset += read;
        }
        uint8_t extra;
        DWORD read = 0;
        if (!current()) return ERROR_OPERATION_ABORTED;
        if (!WinHttpReadData(r, &extra, 1, &read)) return GetLastError();
        return read ? ERROR_INVALID_DATA : ERROR_SUCCESS;
    }
};

std::wstring deviceName() {
    wchar_t name[256];
    DWORD n = 256;
    GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n);
    return name;
}

void uploadWorker(std::wstring path, std::wstring host, std::wstring cookie, std::wstring token, uint64_t ticket) {
    workerGeneration = ticket;
    if (!valid(ticket)) return;
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size{};
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size)) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        postStatus(L"无法读取 " + name);
        return;
    }
    Req q(ticket);
    bool ok = q.open(L"PUT", host, name, cookie, deviceName(), token);
    std::wstring len = L"Content-Type: application/octet-stream\r\nContent-Length: " + std::to_wstring(size.QuadPart) + L"\r\n";
    ok = ok && WinHttpAddRequestHeaders(q.r, len.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    ok = ok && q.send(WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH);
    std::vector<char> buf(256 * 1024);
    long long sent = 0;
    int lastPct = -1;
    while (ok && valid(ticket)) {
        DWORD read = 0;
        if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        DWORD wrote = 0;
        ok = q.current() && WinHttpWriteData(q.r, buf.data(), read, &wrote) && wrote == read;
        sent += read;
        int pct = size.QuadPart ? (int)(sent * 100 / size.QuadPart) : 100;
        if (pct / 10 != lastPct / 10) {
            lastPct = pct;
            postStatus(fmt(L"正在上传 %ls：%d%%", name.c_str(), pct));
        }
    }
    CloseHandle(f);
    ok = ok && q.receive();
    DWORD code = ok ? q.status() : 0;
    if (code == 200) postStatus(L"已上传 " + name);
    else postStatus(fmt(L"上传 %ls 失败：%ls", name.c_str(), code == 413 ? L"文件过大" : code ? fmt(L"HTTP %lu", code).c_str() : fmt(L"网络错误 %lu", GetLastError()).c_str()));
}

void downloadWorker(std::wstring name, std::wstring host, std::wstring cookie, std::wstring token, uint64_t ticket) {
    workerGeneration = ticket;
    if (!valid(ticket)) return;
    PWSTR dl = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl))) dir = dl;
    CoTaskMemFree(dl);
    std::wstring dest = dir + L"\\" + name;
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot), ext = stem == name ? L"" : name.substr(dot);
    for (int n = 2; GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES; n++) dest = dir + L"\\" + stem + fmt(L" (%d)", n) + ext;
    Req q(ticket);
    bool ok = q.open(L"GET", host, name, cookie, deviceName(), token) && q.send() && q.receive();
    DWORD code = ok ? q.status() : 0;
    if (code != 200) {
        postStatus(fmt(L"下载 %ls 失败：%ls", name.c_str(), code ? fmt(L"HTTP %lu", code).c_str() : L"网络错误"));
        return;
    }
    static std::atomic<uint64_t> nextPart{0};
    std::wstring tmp = dest + L".part-" + std::to_wstring(++nextPart);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { postStatus(L"无法写入 " + dest); return; }
    std::vector<char> buf(256 * 1024);
    while (ok && valid(ticket)) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(q.r, &avail)) { ok = false; break; }
        if (!avail) break;
        DWORD read = 0, wrote = 0;
        ok = q.current() && WinHttpReadData(q.r, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &read) &&
             q.current() && WriteFile(f, buf.data(), read, &wrote, nullptr) && wrote == read;
    }
    CloseHandle(f);
    std::unique_lock<std::mutex> commit(activityMutex);
    if (!ok || !valid(ticket) || !MoveFileExW(tmp.c_str(), dest.c_str(), 0)) {
        DeleteFileW(tmp.c_str());
        postStatus(L"下载 " + name + L" 失败");
        return;
    }
    commit.unlock();
    postStatus(L"已下载到 " + dest);
    auto *path = new std::wstring(dest);
    if (!PostMessageW(parentWnd, WM_APP_FILES_REVEAL, (WPARAM)ticket, (LPARAM)path)) delete path;
}

void deleteWorker(std::wstring name, std::wstring host, std::wstring cookie, std::wstring token, uint64_t ticket) {
    workerGeneration = ticket;
    if (!valid(ticket)) return;
    Req q(ticket);
    bool ok = q.open(L"DELETE", host, name, cookie, deviceName(), token) && q.send() && q.receive();
    DWORD code = ok ? q.status() : 0;
    postStatus(code == 204 || code == 404 ? L"已删除 " + name : fmt(L"删除 %ls 失败（%lu）", name.c_str(), code));
}

std::wstring host() { return L"lzc-mesh." + widen(g.boxDomain); }

int selected() { return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1; }
}  // namespace

HWND create(HWND parent, HFONT font) {
    parentWnd = parent;
    // Flat list: no 3D border, no grid lines; the panel draws the frame.
    list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)ID_FILES_LIST, nullptr, nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(list, L"Explorer", nullptr);
    ListView_SetBkColor(list, RGB(255, 255, 255));
    ListView_SetTextBkColor(list, RGB(255, 255, 255));
    ListView_SetTextColor(list, RGB(17, 17, 17));
    const wchar_t *cols[] = {L"文件名", L"大小", L"来自", L"时间"};
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i];
        c.cx = 100;
        ListView_InsertColumn(list, i, &c);
    }
    SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
    ShowWindow(list, SW_HIDE);   // shown once there are files
    DragAcceptFiles(parent, TRUE);
    return list;
}

void setFont(HFONT font) { SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE); }

const std::wstring &status() { return statusText; }

bool empty() { return current.empty(); }

void layout(int x, int y, int w, int h) {
    MoveWindow(list, x, y, w, h, TRUE);
    int other = MulDiv(w, 18, 100);
    ListView_SetColumnWidth(list, 0, std::max(120, w - 3 * other - GetSystemMetrics(SM_CXVSCROLL)));
    for (int i = 1; i < 4; i++) ListView_SetColumnWidth(list, i, other);
}

void update(const std::vector<SharedFile> &files) {
    current = files;
    ShowWindow(list, files.empty() ? SW_HIDE : SW_SHOW);
    InvalidateRect(parentWnd, nullptr, FALSE);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < files.size(); i++) {
        const SharedFile &f = files[i];
        std::wstring name = widen(f.name), size = sizeText(f.size), from = widen(f.uploader.empty() ? "-" : f.uploader);
        FILETIME ft;
        ULONGLONG t = (ULONGLONG)f.modified * 10000000ULL + 116444736000000000ULL;
        ft.dwLowDateTime = (DWORD)t;
        ft.dwHighDateTime = (DWORD)(t >> 32);
        FILETIME local;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&ft, &local);
        FileTimeToSystemTime(&local, &st);
        std::wstring when = fmt(L"%02d-%02d %02d:%02d", st.wMonth, st.wDay, st.wHour, st.wMinute);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = name.data();
        ListView_InsertItem(list, &it);
        ListView_SetItemText(list, (int)i, 1, size.data());
        ListView_SetItemText(list, (int)i, 2, from.data());
        ListView_SetItemText(list, (int)i, 3, when.data());
    }
}

void setStatusText(const std::wstring &t) {
    statusText = t;
    InvalidateRect(parentWnd, nullptr, FALSE);
}

void setActive(bool on) {
    std::lock_guard<std::mutex> lock(activityMutex);
    if (active == on) return;
    ++generation;
    active = on;
}
bool accepts(uint64_t ticket) { return valid(ticket); }

bool ready() {
    if (!g.operational() || g.boxDomain.empty()) {
        setStatusText(g.deviceReady && !g.deviceEnabled ? L"客户端已关闭，开启后才能共享文件" : L"连接中枢并确认设备状态后才能共享文件");
        return false;
    }
    return true;
}

void cancelClipboardRequests() {
    ++imageGeneration;
}

void requestClipboardImage(std::shared_ptr<const ImageBytes> png, const std::string &id, size_t size,
                           std::function<void(ClipboardImageResponse)> complete) {
    const auto path = id.empty() ? std::wstring(L"/api/clipboard/image") : L"/api/clipboard/image/" + widen(id);
    std::thread([png = std::move(png), size, path, complete = std::move(complete), server = host(),
                 cookie = widen(g.cookie), token = widen(g.fileToken),
                 ticket = generation.load(), imageTicket = imageGeneration.load()] {
        Req request(ticket, imageTicket);
        const bool upload = bool(png);
        const bool sent = request.openPath(upload ? L"PUT" : L"GET", server, path, cookie, deviceName(), token) &&
            (upload ? request.sendImage(*png) : request.send() && request.receive());
        ClipboardImageResponse response;
        response.status = sent ? request.status() : 0;
        if (!response.status) response.error = GetLastError();
        else if (response.status == (upload ? 201u : 200u)) {
            response.error = request.readImageResponse(response.body, upload ? 43 : size);
            if (response.error) response.body.clear();
        }
        if (request.current()) complete(std::move(response));
    }).detach();
}

void uploadClipboardImage(std::shared_ptr<const ImageBytes> png) {
    if (!ready()) return;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return;
    wchar_t suffix[40]{};
    if (!StringFromGUID2(guid, suffix, 40)) return;
    std::wstring name = std::wstring(L"clipboard-") + std::wstring(suffix + 1, 36) + L".png";
    std::thread([png = std::move(png), name = std::move(name), server = host(),
                 cookie = widen(g.cookie), token = widen(g.fileToken), ticket = generation.load()] {
        workerGeneration = ticket;
        Req request(ticket);
        postStatus(L"正在上传 " + name);
        const bool sent = request.open(L"PUT", server, name, cookie, deviceName(), token) && request.sendImage(*png);
        const DWORD status = sent ? request.status() : 0;
        postStatus(status == 200 ? L"已上传 " + name : fmt(L"上传 %ls 失败（HTTP %lu）", name.c_str(), status));
    }).detach();
}

void uploadPaths(const std::vector<std::wstring> &paths) {
    if (!ready()) return;
    for (auto &p : paths) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::thread(uploadWorker, p, host(), widen(g.cookie), widen(g.fileToken), generation.load()).detach();
    }
}

void pickAndUpload() {
    if (!ready()) return;
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW o{sizeof o};
    o.hwndOwner = parentWnd;
    o.lpstrFile = buf.data();
    o.nMaxFile = (DWORD)buf.size();
    o.lpstrTitle = L"选择要共享的文件";
    o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&o)) return;
    std::vector<std::wstring> parts;
    for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1) parts.push_back(p);
    std::vector<std::wstring> paths;
    if (parts.size() == 1) paths = parts;
    else for (size_t i = 1; i < parts.size(); i++) paths.push_back(parts[0] + L"\\" + parts[i]);
    uploadPaths(paths);
}

void onDrop(HDROP drop) {
    std::vector<std::wstring> paths;
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; i++) {
        wchar_t p[MAX_PATH * 4];
        if (DragQueryFileW(drop, i, p, MAX_PATH * 4)) paths.push_back(p);
    }
    DragFinish(drop);
    uploadPaths(paths);
}

void downloadSelected() {
    int i = selected();
    if (i < 0 || i >= (int)current.size()) { setStatusText(L"先在列表中选择一个文件"); return; }
    if (!ready()) return;
    setStatusText(L"正在下载 " + widen(current[i].name) + L"…");
    std::thread(downloadWorker, widen(current[i].name), host(), widen(g.cookie), widen(g.fileToken), generation.load()).detach();
}

void deleteSelected() {
    int i = selected();
    if (i < 0 || i >= (int)current.size()) { setStatusText(L"先在列表中选择一个文件"); return; }
    if (!ready()) return;
    std::wstring name = widen(current[i].name);
    if (MessageBoxW(parentWnd, (L"删除“" + name + L"”？所有设备上都将无法再下载它。").c_str(), L"懒猫设备接力", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    if (ready()) std::thread(deleteWorker, name, host(), widen(g.cookie), widen(g.fileToken), generation.load()).detach();
}
}  // namespace files
