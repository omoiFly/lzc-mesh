#include "clipboard_file.h"

#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <new>
#include <utility>

namespace clipboard {
namespace {
enum ImageFormat { Png, DibV5, Dib, Descriptor, Contents, DropEffect, FormatCount };

HRESULT copyToGlobal(STGMEDIUM &medium, const void *data, size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return E_OUTOFMEMORY;
    void *buffer = GlobalLock(memory);
    if (!buffer) {
        GlobalFree(memory);
        return E_OUTOFMEMORY;
    }
    std::memcpy(buffer, data, size);
    GlobalUnlock(memory);
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = memory;
    return S_OK;
}

HRESULT copyBitmap(STGMEDIUM &medium, HGLOBAL dib) {
    const void *data = GlobalLock(dib);
    if (!data) return STG_E_READFAULT;
    const HRESULT status = copyToGlobal(medium, data, GlobalSize(dib));
    GlobalUnlock(dib);
    return status;
}

class ImageDataObject final : public IDataObject {
public:
    ImageDataObject(UINT pngFormat, const std::shared_ptr<files::ImageBytes> &png, HGLOBAL dibV5, HGLOBAL dib)
        : png(png), dibV5(dibV5), dib(dib) {
        formats[Png] = {static_cast<CLIPFORMAT>(pngFormat), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        formats[DibV5] = {CF_DIBV5, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        formats[Dib] = {static_cast<CLIPFORMAT>(dib ? CF_DIB : 0), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        formats[Descriptor] = {registerFormat(CFSTR_FILEDESCRIPTORW), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        formats[Contents] = {registerFormat(CFSTR_FILECONTENTS), nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM | TYMED_HGLOBAL};
        formats[DropEffect] = {registerFormat(CFSTR_PREFERREDDROPEFFECT), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};

        SYSTEMTIME now{};
        GetLocalTime(&now);
        descriptor.cItems = 1;
        auto &file = descriptor.fgd[0];
        file.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_UNICODE;
        file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        file.nFileSizeLow = static_cast<DWORD>(png->size());
        std::swprintf(file.cFileName, std::size(file.cFileName),
                      L"Screenshot-%04u%02u%02u-%02u%02u%02u-%03u.png",
                      unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
                      unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond), unsigned(now.wMilliseconds));
    }

    bool hasFileFormats() const { return formats[Descriptor].cfFormat && formats[Contents].cfFormat; }

    void takeBitmaps(HGLOBAL &sourceV5, HGLOBAL &sourceDib) {
        sourceV5 = sourceDib = nullptr;
        ownsBitmaps = true;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
        *object = static_cast<IDataObject *>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *request, STGMEDIUM *medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        const HRESULT status = QueryGetData(request);
        if (FAILED(status)) return status;
        switch (findFormat(request->cfFormat)) {
        case Descriptor:
            return copyToGlobal(*medium, &descriptor, sizeof descriptor);
        case Contents:
            if (request->tymed & TYMED_ISTREAM) {
                IStream *stream = SHCreateMemStream(png->data(), static_cast<UINT>(png->size()));
                if (!stream) return E_OUTOFMEMORY;
                const HRESULT seek = stream->Seek({}, STREAM_SEEK_END, nullptr);
                if (FAILED(seek)) {
                    stream->Release();
                    return seek;
                }
                medium->tymed = TYMED_ISTREAM;
                medium->pstm = stream;
                return S_OK;
            }
            return copyToGlobal(*medium, png->data(), png->size());
        case Png:
            return copyToGlobal(*medium, png->data(), png->size());
        case DibV5:
            return copyBitmap(*medium, dibV5);
        case Dib:
            return copyBitmap(*medium, dib);
        case DropEffect: {
            const DWORD effect = DROPEFFECT_COPY;
            return copyToGlobal(*medium, &effect, sizeof effect);
        }
        default:
            return DV_E_FORMATETC;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *request) override {
        if (!request) return E_POINTER;
        if (request->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (request->ptd) return DV_E_DVTARGETDEVICE;
        const auto format = findFormat(request->cfFormat);
        if (format == FormatCount) return DV_E_FORMATETC;
        if (request->lindex != -1 && (format != Contents || request->lindex != 0)) return DV_E_LINDEX;
        if (!(request->tymed & formats[format].tymed)) return DV_E_TYMED;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **enumerator) override {
        if (!enumerator) return E_POINTER;
        *enumerator = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::array<FORMATETC, FormatCount> available{};
        UINT count = 0;
        for (const auto &format : formats) {
            if (format.cfFormat) available[count++] = format;
        }
        return SHCreateStdEnumFmtEtc(count, available.data(), enumerator);
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *output) override {
        if (!output) return E_POINTER;
        output->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ~ImageDataObject() {
        if (!ownsBitmaps) return;
        if (dibV5) GlobalFree(dibV5);
        if (dib) GlobalFree(dib);
    }

    static CLIPFORMAT registerFormat(const wchar_t *name) {
        return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name));
    }

    size_t findFormat(CLIPFORMAT format) const {
        if (format) {
            for (size_t index = 0; index < formats.size(); ++index) {
                if (formats[index].cfFormat == format) return index;
            }
        }
        return FormatCount;
    }

    std::atomic<ULONG> references{1};
    std::shared_ptr<const files::ImageBytes> png;
    HGLOBAL dibV5, dib;
    bool ownsBitmaps = false;
    std::array<FORMATETC, FormatCount> formats{};
    FILEGROUPDESCRIPTORW descriptor{};
};
}

HRESULT setImageClipboard(UINT pngFormat, const std::shared_ptr<files::ImageBytes> &png,
                          HGLOBAL &dibV5, HGLOBAL &dib) {
    auto *data = new (std::nothrow) ImageDataObject(pngFormat, png, dibV5, dib);
    if (!data) return E_OUTOFMEMORY;
    const HRESULT status = data->hasFileFormats() ? OleSetClipboard(data) : E_FAIL;
    if (SUCCEEDED(status)) data->takeBitmaps(dibV5, dib);
    data->Release();
    return status;
}
}
