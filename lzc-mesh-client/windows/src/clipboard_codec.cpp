#include "clipboard_codec.h"

#include <shlwapi.h>
#include <wincodec.h>

#include "../../transport/clipboard_png.h"

namespace clipboard {
namespace {
struct Apartment {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(initialized)) CoUninitialize(); }
};

struct Release {
    void operator()(IUnknown *value) const { if (value) value->Release(); }
};

template <class Interface>
using ComPtr = std::unique_ptr<Interface, Release>;

ComPtr<IWICImagingFactory> factory() {
    IWICImagingFactory *result = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, reinterpret_cast<void **>(&result)))) return {};
    return ComPtr<IWICImagingFactory>(result);
}
}

files::ImageBytes encodeClipboardBitmap(HBITMAP bitmap) {
    Apartment apartment;
    auto imaging = factory();
    if (!imaging) return {};
    IWICBitmap *rawBitmap = nullptr;
    if (FAILED(imaging->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &rawBitmap))) return {};
    ComPtr<IWICBitmap> source(rawBitmap);
    IStream *rawStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &rawStream))) return {};
    ComPtr<IStream> stream(rawStream);
    IWICBitmapEncoder *rawEncoder = nullptr;
    if (FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &rawEncoder))) return {};
    ComPtr<IWICBitmapEncoder> encoder(rawEncoder);
    IWICBitmapFrameEncode *rawFrame = nullptr;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&rawFrame, nullptr))) return {};
    ComPtr<IWICBitmapFrameEncode> frame(rawFrame);
    UINT width = 0, height = 0;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppRGB;
    if (FAILED(source->GetSize(&width, &height)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format))) return {};
    IWICBitmapSource *rawConverted = nullptr;
    if (FAILED(WICConvertBitmapSource(format, source.get(), &rawConverted))) return {};
    ComPtr<IWICBitmapSource> converted(rawConverted);
    if (FAILED(frame->WriteSource(converted.get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) return {};
    STATSTG stat{};
    HGLOBAL memory = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > MAXDWORD ||
        FAILED(GetHGlobalFromStream(stream.get(), &memory))) return {};
    const auto *bytes = static_cast<const uint8_t *>(GlobalLock(memory));
    if (!bytes) return {};
    files::ImageBytes png(bytes, bytes + stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return png;
}

HGLOBAL decodeClipboardBitmap(const files::ImageBytes &png) {
    if (mesh_clipboard_png_size(png.data(), png.size()) != 1) return nullptr;
    Apartment apartment;
    auto imaging = factory();
    ComPtr<IStream> stream(SHCreateMemStream(png.data(), UINT(png.size())));
    IWICBitmapDecoder *rawDecoder = nullptr;
    if (!imaging || !stream || FAILED(imaging->CreateDecoderFromStream(
            stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, &rawDecoder))) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder(rawDecoder);
    IWICBitmapFrameDecode *rawFrame = nullptr;
    if (FAILED(decoder->GetFrame(0, &rawFrame))) return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame(rawFrame);
    IWICFormatConverter *rawConverter = nullptr;
    if (FAILED(imaging->CreateFormatConverter(&rawConverter))) return nullptr;
    ComPtr<IWICFormatConverter> converter(rawConverter);
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        width > 7680 || height > 7680 || (width > 4320 && height > 4320)) return nullptr;
    const UINT stride = width * 4, bytes = stride * height;
    HGLOBAL result = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPV5HEADER) + bytes);
    if (!result) return nullptr;
    auto *header = static_cast<BITMAPV5HEADER *>(GlobalLock(result));
    if (!header) { GlobalFree(result); return nullptr; }
    header->bV5Size = sizeof(*header);
    header->bV5Width = LONG(width);
    header->bV5Height = -LONG(height);
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = bytes;
    header->bV5RedMask = 0x00ff0000;
    header->bV5GreenMask = 0x0000ff00;
    header->bV5BlueMask = 0x000000ff;
    header->bV5AlphaMask = 0xff000000;
    header->bV5CSType = LCS_sRGB;
    header->bV5Intent = LCS_GM_IMAGES;
    const auto status = converter->CopyPixels(nullptr, stride, bytes, reinterpret_cast<BYTE *>(header + 1));
    GlobalUnlock(result);
    if (FAILED(status)) { GlobalFree(result); return nullptr; }
    return result;
}

HGLOBAL createLegacyClipboardBitmap(HGLOBAL dibV5) {
    const auto *source = static_cast<const BITMAPV5HEADER *>(GlobalLock(dibV5));
    if (!source) return nullptr;
    const UINT width = source->bV5Width, height = -source->bV5Height;
    const UINT stride = (width * 3 + 3) & ~3u;
    const UINT bytes = stride * height;
    HGLOBAL result = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPINFOHEADER) + bytes);
    auto *header = result ? static_cast<BITMAPINFOHEADER *>(GlobalLock(result)) : nullptr;
    if (!header) {
        if (result) GlobalFree(result);
        GlobalUnlock(dibV5);
        return nullptr;
    }
    header->biSize = sizeof(*header);
    header->biWidth = LONG(width);
    header->biHeight = LONG(height);
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = bytes;

    const auto *sourcePixels = reinterpret_cast<const BYTE *>(source + 1);
    auto *pixels = reinterpret_cast<BYTE *>(header + 1);
    for (UINT row = 0; row < height; ++row) {
        const auto *input = sourcePixels + (height - 1 - row) * width * 4;
        auto *output = pixels + row * stride;
        for (UINT column = 0; column < width; ++column) {
            memcpy(output + column * 3, input + column * 4, 3);
        }
    }
    GlobalUnlock(result);
    GlobalUnlock(dibV5);
    return result;
}
}
