#include "TextureTranscodeAdapter.h"

#include <ktx.h>
#include "model_core/Ktx2Preflight.h"
#include <cstring>

namespace import_worker {

namespace {

using model_core::PixelFormatId;

// Guarantees ktxTexture2_Destroy runs on every exit path, including the
// early-return failure cases below.
class KtxTexture2Guard {
public:
    explicit KtxTexture2Guard(ktxTexture2* texture)
        : texture_(texture)
    {
    }
    ~KtxTexture2Guard()
    {
        if (texture_ != nullptr) {
            ktxTexture2_Destroy(texture_);
        }
    }
    KtxTexture2Guard(const KtxTexture2Guard&) = delete;
    KtxTexture2Guard& operator=(const KtxTexture2Guard&) = delete;

    ktxTexture2* get() const { return texture_; }

private:
    ktxTexture2* texture_;
};

std::optional<TranscodedImage> CopyKtxLevels(ktxTexture2* texture, PixelFormatId format,
                                            const TextureDecodeOptions& options)
{
    if (options.Cancelled()) return std::nullopt;
    TranscodedImage result;
    uint32_t first=0;
    while (((std::max)(1u,texture->baseWidth>>first)>options.maxDimension
        || (std::max)(1u,texture->baseHeight>>first)>options.maxDimension) && first+1<texture->numLevels) ++first;
    result.width=(std::max)(1u,texture->baseWidth>>first);
    result.height=(std::max)(1u,texture->baseHeight>>first);
    if (result.width>options.maxDimension || result.height>options.maxDimension) return std::nullopt;
    result.pixelFormat=format; result.mipLevels=texture->numLevels-first;
    const auto total=model_core::ComputeImagePixelBytes(format,result.width,result.height,result.mipLevels);
    if (!total || *total>options.maxDecodedBytes) return std::nullopt;
    const auto* data=ktxTexture_GetData(ktxTexture(texture));
    const auto dataSize=ktxTexture_GetDataSize(ktxTexture(texture));
    if (!data) return std::nullopt;
    result.pixelBytes.reserve(static_cast<size_t>(*total));
    for (uint32_t level=first;level<texture->numLevels;++level) {
        if (options.Cancelled()) return std::nullopt;
        ktx_size_t offset=0;
        const auto expected=model_core::ComputeImagePixelBytes(format,(std::max)(1u,texture->baseWidth>>level),
                                                              (std::max)(1u,texture->baseHeight>>level),1);
        const auto size=ktxTexture_GetImageSize(ktxTexture(texture),level);
        if (!expected || *expected!=size || ktxTexture_GetImageOffset(ktxTexture(texture),level,0,0,&offset)!=KTX_SUCCESS
            || offset>dataSize || size>dataSize-offset) return std::nullopt;
        const auto* begin=reinterpret_cast<const std::byte*>(data+offset);
        result.pixelBytes.insert(result.pixelBytes.end(),begin,begin+size);
    }
    if (result.mipLevels==1 && (result.width>64 || result.height>64) && format==PixelFormatId::RGBA8_UNORM) {
        const auto space=options.semantic==TextureSemantic::Color || options.semantic==TextureSemantic::Emissive
            ? model_core::ColorSpaceId::Srgb : model_core::ColorSpaceId::Linear;
        if (!GenerateRasterMips(result.pixelBytes,result.width,result.height,space,options,result.mipLevels)) return std::nullopt;
    }
    return result;
}

} // namespace

std::optional<TranscodedImage> TranscodeKtx2BasisImage(std::span<const std::byte> bytes,
                                                    const TextureDecodeOptions& options)
{
    // Preflight the fixed KTX2 header and level index before the library can
    // allocate/decompress attacker-controlled storage (including Zstd
    // expansion). Shared with the provider path (SEC-02).
    if (options.Cancelled()) return std::nullopt;
    const model_core::Ktx2Limits limits{options.maxEncodedBytes, options.maxDecodedBytes,
                                        options.maxPixels, model_core::kMaxTextureDimension};
    const auto preflight = model_core::PreflightKtx2(bytes, limits);
    if (!preflight.has_value() || options.Cancelled()) return std::nullopt;
    const uint32_t width = preflight->header.width;
    const uint32_t height = preflight->header.height;
    const uint32_t levels = preflight->header.levelCount;
    PixelFormatId format = preflight->format;
    ktxTexture2* raw=nullptr;
    model_core::Ktx2DecoderInvocations().fetch_add(1, std::memory_order_relaxed);
    if (ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(bytes.data()),bytes.size(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&raw)!=KTX_SUCCESS || !raw) return std::nullopt;
    KtxTexture2Guard guard(raw);
    auto* texture=guard.get();
    if (texture->baseWidth!=width || texture->baseHeight!=height || texture->numLevels!=levels
        || texture->numDimensions!=2 || texture->numFaces!=1 || texture->isArray || options.Cancelled()) return std::nullopt;
    if (ktxTexture2_NeedsTranscoding(texture)) {
        // BC5 consumes the Basis green/alpha swizzle; ordinary glTF normal
        // images do not promise that packing. RGBA is the validated semantic
        // fallback for normal/data; BC7 preserves all color/emissive channels.
        format = options.semantic==TextureSemantic::Normal || options.semantic==TextureSemantic::Data
            || (levels==1 && (width>64 || height>64))
            ? PixelFormatId::RGBA8_UNORM : PixelFormatId::BC7_UNORM;
        auto target = format==PixelFormatId::BC7_UNORM ? KTX_TTF_BC7_RGBA : KTX_TTF_RGBA32;
        if (ktxTexture2_TranscodeBasis(texture,target,0)!=KTX_SUCCESS) {
            // Reload: a failed transcode need not leave the texture untouched.
            ktxTexture2* retryRaw=nullptr;
            if (options.Cancelled() || ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(bytes.data()),
                bytes.size(),KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&retryRaw)!=KTX_SUCCESS || !retryRaw) return std::nullopt;
            KtxTexture2Guard retry(retryRaw);
            if (ktxTexture2_TranscodeBasis(retryRaw,KTX_TTF_RGBA32,0)!=KTX_SUCCESS) return std::nullopt;
            return CopyKtxLevels(retryRaw,PixelFormatId::RGBA8_UNORM,options);
        }
    } else if (format==PixelFormatId::Unknown || (format==PixelFormatId::BC5_UNORM
        && (options.semantic==TextureSemantic::Color || options.semantic==TextureSemantic::Emissive))) return std::nullopt;
    return CopyKtxLevels(texture,format,options);
}

} // namespace import_worker
