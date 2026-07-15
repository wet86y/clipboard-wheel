#include "platform/windows/ImageClipboardPayload.h"

#include <bcrypt.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace smk::windows {
namespace {
using Microsoft::WRL::ComPtr;

ComPtr<IWICImagingFactory> wic_factory() {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()))))
        CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    return factory;
}

std::wstring hash_bytes(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, hash_size = 0, transferred = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &transferred, 0) < 0
        || BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &transferred, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); return {};
    }
    std::vector<std::uint8_t> object(object_size), digest(hash_size);
    const bool ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0
        && BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0
        && BCryptFinishHash(hash, digest.data(), hash_size, 0) >= 0;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr wchar_t digits[] = L"0123456789ABCDEF"; std::wstring result; result.reserve(digest.size() * 2);
    for (auto value : digest) { result.push_back(digits[value >> 4]); result.push_back(digits[value & 15]); }
    return result;
}

bool valid_dimensions(UINT width, UINT height) {
    return width > 0 && height > 0 && static_cast<std::uint64_t>(width) * height <= ImageClipboardPayload::kMaxPixels;
}

std::vector<std::uint8_t> stream_bytes(IStream* stream) {
    if (!stream) return {};
    STATSTG stat{}; if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart <= 0
        || stat.cbSize.QuadPart > static_cast<LONGLONG>(ImageClipboardPayload::kMaxEncodedBytes)) return {};
    LARGE_INTEGER zero{}; stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(stat.cbSize.QuadPart)); ULONG read = 0;
    if (FAILED(stream->Read(result.data(), static_cast<ULONG>(result.size()), &read)) || read != result.size()) return {};
    return result;
}

std::vector<std::uint8_t> global_bytes(HGLOBAL global) {
    const SIZE_T size = global ? GlobalSize(global) : 0;
    if (!size || size > ImageClipboardPayload::kMaxEncodedBytes) return {};
    const auto* memory = static_cast<const std::uint8_t*>(GlobalLock(global));
    if (!memory) return {}; std::vector<std::uint8_t> result(memory, memory + size); GlobalUnlock(global); return result;
}

std::vector<std::uint8_t> medium_bytes(const STGMEDIUM& medium) {
    if (medium.tymed == TYMED_HGLOBAL) return global_bytes(medium.hGlobal);
    if (medium.tymed == TYMED_ISTREAM) return stream_bytes(medium.pstm);
    return {};
}

bool decode_encoded(const std::vector<std::uint8_t>& encoded, UINT& width, UINT& height, std::vector<std::uint8_t>& pixels) {
    if (encoded.empty() || encoded.size() > ImageClipboardPayload::kMaxEncodedBytes) return false;
    const auto factory = wic_factory(); ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    if (!factory || FAILED(factory->CreateStream(stream.GetAddressOf()))
        || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size())))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))
        || FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || FAILED(frame->GetSize(&width, &height))
        || !valid_dimensions(width, height) || FAILED(factory->CreateFormatConverter(converter.GetAddressOf()))
        || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

bool decode_bitmap(HBITMAP bitmap, UINT& width, UINT& height, std::vector<std::uint8_t>& pixels) {
    if (!bitmap) return false; BITMAP details{};
    if (!GetObjectW(bitmap, sizeof(details), &details)) return false;
    width = static_cast<UINT>(std::abs(details.bmWidth)); height = static_cast<UINT>(std::abs(details.bmHeight));
    if (!valid_dimensions(width, height)) return false;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height); info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    pixels.resize(static_cast<std::size_t>(width) * height * 4); HDC dc = GetDC(nullptr);
    const bool ok = GetDIBits(dc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) == static_cast<int>(height);
    ReleaseDC(nullptr, dc); return ok;
}

bool decode_dib(const std::vector<std::uint8_t>& dib, UINT& width, UINT& height, std::vector<std::uint8_t>& pixels) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biSize > dib.size() || header->biWidth <= 0 || header->biHeight == 0) return false;
    width = static_cast<UINT>(header->biWidth); height = static_cast<UINT>(std::abs(header->biHeight));
    if (!valid_dimensions(width, height)) return false;
    std::size_t colors = header->biClrUsed;
    if (!colors && header->biBitCount <= 8) colors = 1ULL << header->biBitCount;
    std::size_t offset = header->biSize + colors * sizeof(RGBQUAD);
    if (header->biCompression == BI_BITFIELDS && header->biSize == sizeof(BITMAPINFOHEADER)) offset += 3 * sizeof(DWORD);
    if (offset >= dib.size()) return false;
    if (header->biBitCount == 32 && (header->biCompression == BI_RGB || header->biCompression == BI_BITFIELDS)) {
        const std::size_t stride = static_cast<std::size_t>(width) * 4;
        if (offset + stride * height > dib.size()) return false;
        pixels.resize(stride * height);
        for (UINT row = 0; row < height; ++row) {
            const UINT source_row = header->biHeight < 0 ? row : height - row - 1;
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * stride,
                dib.data() + offset + static_cast<std::size_t>(source_row) * stride, stride);
        }
        return true;
    }
    BITMAPV5HEADER destination{}; destination.bV5Size = sizeof(destination); destination.bV5Width = static_cast<LONG>(width);
    destination.bV5Height = -static_cast<LONG>(height); destination.bV5Planes = 1; destination.bV5BitCount = 32; destination.bV5Compression = BI_RGB;
    void* target_pixels = nullptr; HDC screen = GetDC(nullptr);
    HBITMAP target = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&destination), DIB_RGB_COLORS, &target_pixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen); HGDIOBJ previous = target && dc ? SelectObject(dc, target) : nullptr;
    const int copied = target && dc ? StretchDIBits(dc, 0, 0, width, height, 0, 0, width, height,
        dib.data() + offset, reinterpret_cast<const BITMAPINFO*>(dib.data()), DIB_RGB_COLORS, SRCCOPY) : 0;
    if (dc && previous) SelectObject(dc, previous); if (dc) DeleteDC(dc); ReleaseDC(nullptr, screen);
    if (!target || copied == GDI_ERROR || !target_pixels) { if (target) DeleteObject(target); return false; }
    pixels.assign(static_cast<std::uint8_t*>(target_pixels), static_cast<std::uint8_t*>(target_pixels) + static_cast<std::size_t>(width) * height * 4);
    DeleteObject(target); return true;
}

void repair_zero_alpha(std::vector<std::uint8_t>& pixels) {
    bool any_alpha = false; for (std::size_t i = 3; i < pixels.size(); i += 4) any_alpha |= pixels[i] != 0;
    if (!any_alpha) for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
}

std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t>& pixels, UINT width, UINT height, UINT output_width = 0, UINT output_height = 0) {
    const auto factory = wic_factory(); if (!factory) return {};
    ComPtr<IWICBitmap> bitmap; ComPtr<IWICBitmapSource> source; ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
        static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()), bitmap.GetAddressOf()))) return {};
    source = bitmap;
    if (output_width && output_height && (output_width != width || output_height != height)) {
        if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf()))
            || FAILED(scaler->Initialize(bitmap.Get(), output_width, output_height, WICBitmapInterpolationModeFant))) return {};
        source = scaler;
    }
    ComPtr<IStream> stream; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> properties;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))
        || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))
        || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
        || FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf()))
        || FAILED(frame->Initialize(properties.Get())) || FAILED(frame->SetSize(output_width ? output_width : width, output_height ? output_height : height))) return {};
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(source.Get(), nullptr))
        || FAILED(frame->Commit()) || FAILED(encoder->Commit())) return {};
    HGLOBAL global = nullptr; return SUCCEEDED(GetHGlobalFromStream(stream.Get(), &global)) ? global_bytes(global) : std::vector<std::uint8_t>{};
}

std::optional<ImageClipboardPayload> make_payload(UINT width, UINT height, std::vector<std::uint8_t> pixels) {
    if (!valid_dimensions(width, height) || pixels.size() != static_cast<std::size_t>(width) * height * 4) return std::nullopt;
    repair_zero_alpha(pixels); ImageClipboardPayload result; result.width = width; result.height = height;
    result.png = encode_png(pixels, width, height); if (result.png.empty() || result.png.size() > ImageClipboardPayload::kMaxEncodedBytes) return std::nullopt;
    const double scale = std::min(1.0, static_cast<double>(ImageClipboardPayload::kPreviewMaxEdge) / std::max(width, height));
    const UINT preview_width = std::max(1U, static_cast<UINT>(std::lround(width * scale)));
    const UINT preview_height = std::max(1U, static_cast<UINT>(std::lround(height * scale)));
    result.preview_png = encode_png(pixels, width, height, preview_width, preview_height);
    result.sha256 = hash_bytes(result.png); return result;
}

HGLOBAL copy_global(const void* data, std::size_t size) {
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size); if (!global) return nullptr;
    void* target = GlobalLock(global); if (!target) { GlobalFree(global); return nullptr; }
    std::memcpy(target, data, size); GlobalUnlock(global); return global;
}

std::vector<std::uint8_t> make_dib(const ImageClipboardPayload& payload, bool v5) {
    UINT width = 0, height = 0; std::vector<std::uint8_t> pixels;
    if (!decode_encoded(payload.png, width, height, pixels)) return {};
    const std::size_t header_size = v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    std::vector<std::uint8_t> result(header_size + pixels.size());
    if (v5) {
        auto* header = reinterpret_cast<BITMAPV5HEADER*>(result.data()); header->bV5Size = sizeof(*header);
        header->bV5Width = static_cast<LONG>(width); header->bV5Height = -static_cast<LONG>(height); header->bV5Planes = 1;
        header->bV5BitCount = 32; header->bV5Compression = BI_BITFIELDS; header->bV5RedMask = 0x00FF0000;
        header->bV5GreenMask = 0x0000FF00; header->bV5BlueMask = 0x000000FF; header->bV5AlphaMask = 0xFF000000;
        header->bV5CSType = LCS_sRGB; header->bV5SizeImage = static_cast<DWORD>(pixels.size());
    } else {
        auto* header = reinterpret_cast<BITMAPINFOHEADER*>(result.data()); header->biSize = sizeof(*header);
        header->biWidth = static_cast<LONG>(width); header->biHeight = -static_cast<LONG>(height); header->biPlanes = 1;
        header->biBitCount = 32; header->biCompression = BI_RGB; header->biSizeImage = static_cast<DWORD>(pixels.size());
    }
    std::memcpy(result.data() + header_size, pixels.data(), pixels.size()); return result;
}

HBITMAP make_bitmap(const ImageClipboardPayload& payload) {
    UINT width = 0, height = 0; std::vector<std::uint8_t> pixels;
    if (!decode_encoded(payload.png, width, height, pixels)) return nullptr;
    BITMAPV5HEADER header{}; header.bV5Size = sizeof(header); header.bV5Width = static_cast<LONG>(width);
    header.bV5Height = -static_cast<LONG>(height); header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_RGB;
    void* target = nullptr; HDC dc = GetDC(nullptr); HBITMAP bitmap = CreateDIBSection(dc,
        reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &target, nullptr, 0); ReleaseDC(nullptr, dc);
    if (bitmap && target) std::memcpy(target, pixels.data(), pixels.size()); else if (bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
    return bitmap;
}

class FormatEnumerator final : public IEnumFORMATETC {
public:
    explicit FormatEnumerator(std::vector<FORMATETC> formats) : formats_(std::move(formats)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC) { *value = this; AddRef(); return S_OK; } return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* output, ULONG* fetched) override {
        if (!output || (count != 1 && !fetched)) return E_POINTER; ULONG copied = 0;
        while (copied < count && position_ < formats_.size()) output[copied++] = formats_[position_++];
        if (fetched) *fetched = copied; return copied == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override { position_ = std::min(formats_.size(), position_ + count); return position_ < formats_.size() ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** result) override {
        if (!result) return E_POINTER; auto* clone = new FormatEnumerator(formats_); clone->position_ = position_; *result = clone; return S_OK;
    }
private:
    std::atomic<ULONG> references_{1}; std::vector<FORMATETC> formats_; std::size_t position_ = 0;
};

class ImageDataObject final : public IDataObject {
public:
    explicit ImageDataObject(ImageClipboardPayload payload) : payload_(std::move(payload)) {
        png_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG"));
        image_png_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"image/png"));
        history_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"));
        formats_ = {{png_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM},
            {image_png_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM},
            {CF_DIBV5, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}, {CF_DIB, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {CF_BITMAP, nullptr, DVASPECT_CONTENT, -1, TYMED_GDI}, {history_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) { *value = this; AddRef(); return S_OK; } return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request, STGMEDIUM* medium) override {
        if (!request || !medium) return E_POINTER; std::memset(medium, 0, sizeof(*medium));
        if (FAILED(QueryGetData(request))) return DV_E_FORMATETC;
        if (request->cfFormat == png_ || request->cfFormat == image_png_) {
            if ((request->tymed & TYMED_ISTREAM) && !(request->tymed & TYMED_HGLOBAL)) {
                medium->tymed = TYMED_ISTREAM; return CreateStreamOnHGlobal(copy_global(payload_.png.data(), payload_.png.size()), TRUE, &medium->pstm);
            }
            medium->tymed = TYMED_HGLOBAL; medium->hGlobal = copy_global(payload_.png.data(), payload_.png.size());
        } else if (request->cfFormat == CF_DIBV5 || request->cfFormat == CF_DIB) {
            const auto bytes = make_dib(payload_, request->cfFormat == CF_DIBV5); medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = copy_global(bytes.data(), bytes.size());
        } else if (request->cfFormat == CF_BITMAP) { medium->tymed = TYMED_GDI; medium->hBitmap = make_bitmap(payload_); }
        else { const BOOL include = FALSE; medium->tymed = TYMED_HGLOBAL; medium->hGlobal = copy_global(&include, sizeof(include)); }
        return (medium->tymed == TYMED_GDI ? medium->hBitmap != nullptr : medium->hGlobal != nullptr) ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
        if (!request || request->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        for (const auto& format : formats_) if (format.cfFormat == request->cfFormat && (format.tymed & request->tymed)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override { if (output) output->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** result) override {
        if (!result) return E_POINTER; if (direction != DATADIR_GET) return E_NOTIMPL; *result = new FormatEnumerator(formats_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    std::atomic<ULONG> references_{1}; ImageClipboardPayload payload_; CLIPFORMAT png_{}, image_png_{}, history_{};
    std::vector<FORMATETC> formats_;
};
}

std::optional<ImageClipboardPayload> normalize_png_payload(const std::vector<std::uint8_t>& png) {
    UINT width = 0, height = 0; std::vector<std::uint8_t> pixels;
    return decode_encoded(png, width, height, pixels) ? make_payload(width, height, std::move(pixels)) : std::nullopt;
}

std::optional<ImageClipboardPayload> normalize_bgra_payload(
    UINT width, UINT height, const std::vector<std::uint8_t>& pixels) {
    return make_payload(width, height, pixels);
}

std::optional<ImageClipboardPayload> read_image_payload(IDataObject* data) {
    if (!data) return std::nullopt;
    const std::array<const wchar_t*, 6> encoded_names{L"PNG", L"image/png", L"JFIF", L"image/jpeg", L"JPEG", L"image/bmp"};
    for (const auto* name : encoded_names) {
        FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM};
        STGMEDIUM medium{}; if (SUCCEEDED(data->GetData(&format, &medium))) {
            const auto bytes = medium_bytes(medium); ReleaseStgMedium(&medium); UINT width = 0, height = 0; std::vector<std::uint8_t> pixels;
            if (decode_encoded(bytes, width, height, pixels)) return make_payload(width, height, std::move(pixels));
        }
    }
    for (const CLIPFORMAT format_id : std::array<CLIPFORMAT, 2>{static_cast<CLIPFORMAT>(CF_DIBV5), static_cast<CLIPFORMAT>(CF_DIB)}) {
        FORMATETC format{format_id, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
        if (SUCCEEDED(data->GetData(&format, &medium))) {
            const auto bytes = global_bytes(medium.hGlobal); ReleaseStgMedium(&medium); UINT width = 0, height = 0; std::vector<std::uint8_t> pixels;
            if (decode_dib(bytes, width, height, pixels)) return make_payload(width, height, std::move(pixels));
        }
    }
    FORMATETC bitmap_format{CF_BITMAP, nullptr, DVASPECT_CONTENT, -1, TYMED_GDI}; STGMEDIUM medium{};
    if (SUCCEEDED(data->GetData(&bitmap_format, &medium))) {
        UINT width = 0, height = 0; std::vector<std::uint8_t> pixels; const bool ok = decode_bitmap(medium.hBitmap, width, height, pixels);
        ReleaseStgMedium(&medium); if (ok) return make_payload(width, height, std::move(pixels));
    }
    return std::nullopt;
}

ComPtr<IDataObject> create_image_data_object(const ImageClipboardPayload& payload) {
    ComPtr<IDataObject> result; if (payload.valid()) result.Attach(new ImageDataObject(payload)); return result;
}

} // namespace smk::windows
