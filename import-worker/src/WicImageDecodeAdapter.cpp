#include "WicImageDecodeAdapter.h"

#include "platform/CheckedMath.h"

#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace import_worker {

namespace {

using model_core::ColorSpaceId;
using model_core::PixelFormatId;
using platform::CheckedMultiply;

// Sanity caps enforced before any WIC allocation -- same discipline every
// other adapter in this repo already follows (e.g. GltfAdapter.cpp's
// kMaxVertices/kMaxIndices). Not part of the wire format; purely
// adapter-side guards against a hostile/malformed input driving an
// unbounded WIC allocation.
constexpr uint64_t kMaxEncodedImageBytes = 256ull * 1024ull * 1024ull;
constexpr uint32_t kMaxImageDimension = 16384;

} // namespace

std::optional<DecodedRasterImage> DecodeRasterImageWic(std::span<const std::byte> encodedBytes,
                                                          ColorSpaceId colorSpace)
{
    if (encodedBytes.empty() || encodedBytes.size() > kMaxEncodedImageBytes) {
        return std::nullopt;
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) {
        return std::nullopt;
    }
    if (FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(encodedBytes.data())),
            static_cast<DWORD>(encodedBytes.size())))) {
        return std::nullopt;
    }

    // Only ever decodes via the inbox-registered decoder WIC itself selects
    // for the container it detects from these bytes -- never enumerates or
    // is handed a caller-chosen decoder CLSID. This is the "does not
    // enumerate or invoke arbitrary installed WIC codecs" texture-policy
    // requirement: the constraint is about not walking the *codec*
    // registry, not about which *container* (PNG/JPEG/BMP/TIFF) WIC
    // recognizes from the bytes -- that recognition is exactly what
    // ImageFormatSniff.h's own caller-side check already independently
    // confirms before this function is ever called.
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                 &decoder))) {
        return std::nullopt;
    }

    UINT frameCount = 0;
    if (FAILED(decoder->GetFrameCount(&frameCount)) || frameCount == 0) {
        return std::nullopt;
    }

    // First frame only -- multi-frame TIFF and animated formats are scoped
    // out of this adapter; a later slice can add explicit multi-frame/
    // animation handling if a real need surfaces.
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return std::nullopt;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 || width > kMaxImageDimension
        || height > kMaxImageDimension) {
        return std::nullopt;
    }
    auto pixelCount = CheckedMultiply(static_cast<uint64_t>(width), static_cast<uint64_t>(height));
    if (!pixelCount) {
        return std::nullopt;
    }

    // WICConvertBitmapSource absorbs WIC's whole pixel-format zoo (indexed
    // palettes, CMYK, 16-bit-per-channel, premultiplied alpha, etc.) into
    // one canonical target -- this adapter never special-cases a WIC pixel
    // format itself.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        return std::nullopt;
    }
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                      nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return std::nullopt;
    }

    auto rowBytes = CheckedMultiply(static_cast<uint64_t>(width), 4ull);
    auto totalBytes = rowBytes ? CheckedMultiply(*rowBytes, static_cast<uint64_t>(height)) : std::nullopt;
    if (!rowBytes || !totalBytes) {
        return std::nullopt;
    }

    DecodedRasterImage result;
    result.width = width;
    result.height = height;
    result.colorSpace = colorSpace;
    result.pixelBytes.resize(*totalBytes);
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(*rowBytes),
                                      static_cast<UINT>(result.pixelBytes.size()),
                                      reinterpret_cast<BYTE*>(result.pixelBytes.data())))) {
        return std::nullopt;
    }

    return result;
}

} // namespace import_worker
