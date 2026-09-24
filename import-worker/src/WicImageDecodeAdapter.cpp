#include "WicImageDecodeAdapter.h"
#include "ImageFormatSniff.h"

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
                                                          ColorSpaceId colorSpace, const TextureDecodeOptions& options)
{
    if (options.Cancelled() || !options.maxDimension || options.maxDimension > kMaxImageDimension
        || encodedBytes.empty() || encodedBytes.size() > (std::min)(kMaxEncodedImageBytes,options.maxEncodedBytes)) {
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

    // Explicit inbox CLSIDs: stream-based factory discovery can invoke a
    // third-party installed codec. Sniff first and never enumerate codecs.
    const CLSID* codec = nullptr;
    switch (SniffImageFormat(encodedBytes)) {
    case SniffedImageFormat::Png: codec=&CLSID_WICPngDecoder; break;
    case SniffedImageFormat::Jpeg: codec=&CLSID_WICJpegDecoder; break;
    case SniffedImageFormat::Bmp: codec=&CLSID_WICBmpDecoder; break;
    case SniffedImageFormat::Tiff: codec=&CLSID_WICTiffDecoder; break;
    default: return std::nullopt;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (options.Cancelled() || FAILED(CoCreateInstance(*codec,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&decoder)))
        || FAILED(decoder->Initialize(stream.Get(),WICDecodeMetadataCacheOnDemand))) {
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
    if (!pixelCount || *pixelCount > options.maxPixels || options.Cancelled()) {
        return std::nullopt;
    }

    // WICConvertBitmapSource absorbs WIC's whole pixel-format zoo (indexed
    // palettes, CMYK, 16-bit-per-channel, premultiplied alpha, etc.) into
    // one canonical target -- this adapter never special-cases a WIC pixel
    // format itself.
    // Inbox JPEG offers native scaled decode through IWICBitmapSourceTransform.
    // Ask only that codec; PNG/BMP/TIFF use bounded tiled full-resolution reads.
    uint32_t outputW=width, outputH=height;
    while (outputW>options.maxDimension || outputH>options.maxDimension) {
        outputW=(std::max)(1u,outputW/2); outputH=(std::max)(1u,outputH/2);
    }
    auto chainBytes=model_core::ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,outputW,outputH,
                                                       model_core::FullImageMipCount(outputW,outputH));
    if (!chainBytes || *chainBytes>options.maxDecodedBytes) return std::nullopt;
    DecodedRasterImage result;
    result.width=outputW; result.height=outputH; result.colorSpace=colorSpace;
    result.pixelBytes.resize(size_t(outputW)*outputH*4);
    ComPtr<IWICBitmapSourceTransform> native;
    bool nativeDecoded=false;
    if (SniffImageFormat(encodedBytes)==SniffedImageFormat::Jpeg
        && SUCCEEDED(frame.As(&native))) {
        UINT nativeW=outputW,nativeH=outputH;
        WICPixelFormatGUID nativeFormat=GUID_WICPixelFormat32bppRGBA;
        if (SUCCEEDED(native->GetClosestSize(&nativeW,&nativeH)) && nativeW==outputW && nativeH==outputH
            && SUCCEEDED(native->GetClosestPixelFormat(&nativeFormat))
            && (nativeFormat==GUID_WICPixelFormat32bppRGBA || nativeFormat==GUID_WICPixelFormat32bppBGR
                || nativeFormat==GUID_WICPixelFormat24bppBGR)) {
            nativeDecoded=true;
            const UINT channels=nativeFormat==GUID_WICPixelFormat24bppBGR ? 3u : 4u;
            std::vector<BYTE> tile(size_t(outputW)*channels*32);
            for (UINT y=0;y<outputH;y+=32) {
                if (options.Cancelled()) return std::nullopt;
                const UINT rows=(std::min)(32u,outputH-y);
                WICRect rectangle{0,static_cast<INT>(y),static_cast<INT>(outputW),static_cast<INT>(rows)};
                if (FAILED(native->CopyPixels(&rectangle,outputW,outputH,&nativeFormat,WICBitmapTransformRotate0,
                    outputW*channels,rows*outputW*channels,tile.data()))) {
                    nativeDecoded=false; break;
                }
                for (size_t i=0;i<size_t(rows)*outputW;++i) {
                    auto* out=result.pixelBytes.data()+(size_t(y)*outputW+i)*4;
                    const bool rgba=nativeFormat==GUID_WICPixelFormat32bppRGBA;
                    out[0]=std::byte(tile[i*channels+(rgba ? 0 : 2)]);
                    out[1]=std::byte(tile[i*channels+1]);out[2]=std::byte(tile[i*channels+(rgba ? 2 : 0)]);
                    out[3]=std::byte{255};
                }
            }
        }
    }
    // Without a verified native downscale, the codec's possible full-frame
    // expansion must stay bounded. A scaler keeps our own reads and the
    // converter's source at the output size, so a 4K PNG no longer has to be
    // rejected outright the way the old flat source-pixel guard did; the
    // decoder still runs under the dimension/pixel caps above plus the
    // worker's Job commit ceiling.
    UINT rasterWidth=width, rasterHeight=height;
    ComPtr<IWICBitmapSource> rasterSource;
    if (!nativeDecoded) {
        if (*pixelCount>options.maxDecodedBytes/4) {
            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(factory->CreateBitmapScaler(&scaler))
                || FAILED(scaler->Initialize(frame.Get(),outputW,outputH,WICBitmapInterpolationModeFant)))
                return std::nullopt;
            rasterSource=scaler;
            rasterWidth=outputW; rasterHeight=outputH;
        } else {
            rasterSource=frame;
        }
    }
    if (!nativeDecoded) {
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter))
            || FAILED(converter->Initialize(rasterSource.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,
                nullptr,0.0,WICBitmapPaletteTypeCustom))) return std::nullopt;
        // Scratch <=2 MiB even for the maximum permitted source width.
        std::vector<std::byte> tile(size_t(rasterWidth)*4*32);
        // Area reduce source pixels in linear light into <=32 output rows.
        for (uint32_t outY=0;outY<outputH;++outY) {
            if (options.Cancelled()) return std::nullopt;
            std::vector<float> sums(size_t(outputW)*4,0.0f);
            const uint32_t y0=outY*rasterHeight/outputH, y1=(outY+1)*rasterHeight/outputH;
            for (uint32_t y=y0;y<y1;y+=32) {
                if (options.Cancelled()) return std::nullopt;
                const uint32_t rows=(std::min)(32u,y1-y);
                WICRect rect{0,static_cast<INT>(y),static_cast<INT>(rasterWidth),static_cast<INT>(rows)};
                if (FAILED(converter->CopyPixels(&rect,rasterWidth*4,rows*rasterWidth*4,reinterpret_cast<BYTE*>(tile.data())))) return std::nullopt;
                for (uint32_t x=0;x<outputW;++x) for (uint32_t sy=0;sy<rows;++sy)
                    for (uint32_t sx=x*rasterWidth/outputW;sx<(x+1)*rasterWidth/outputW;++sx) for (unsigned c=0;c<4;++c) {
                        float v=float(std::to_integer<uint8_t>(tile[(size_t(sy)*rasterWidth+sx)*4+c]))/255.0f;
                        if (colorSpace==ColorSpaceId::Srgb && c<3) v=v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f);
                        sums[size_t(x)*4+c]+=v;
                    }
            }
            for (uint32_t x=0;x<outputW;++x) for (unsigned c=0;c<4;++c) {
                float v=sums[size_t(x)*4+c]/float(((x+1)*rasterWidth/outputW-x*rasterWidth/outputW)*(y1-y0));
                if (colorSpace==ColorSpaceId::Srgb && c<3) v=v<=0.0031308f ? v*12.92f : 1.055f*std::pow(v,1.0f/2.4f)-0.055f;
                result.pixelBytes[(size_t(outY)*outputW+x)*4+c]=std::byte(uint8_t(std::clamp(std::lround(v*255),0l,255l)));
            }
        }
    }
    if (!GenerateRasterMips(result.pixelBytes,outputW,outputH,colorSpace,options,result.mipLevels)) return std::nullopt;
    return result;
}

} // namespace import_worker
