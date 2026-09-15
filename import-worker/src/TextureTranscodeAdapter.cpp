#include "TextureTranscodeAdapter.h"

#include <ktx.h>
#include "ImageFormatSniff.h"
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
    // allocate/decompress attacker-controlled storage (including Zstd expansion).
    if (options.Cancelled() || bytes.size()<80 || bytes.size()>options.maxEncodedBytes
        || SniffImageFormat(bytes)!=SniffedImageFormat::Ktx2 || !options.maxDimension) return std::nullopt;
    auto u32=[&](size_t offset) { uint32_t v; std::memcpy(&v,bytes.data()+offset,4); return v; };
    auto u64=[&](size_t offset) { uint64_t v; std::memcpy(&v,bytes.data()+offset,8); return v; };
    const uint32_t width=u32(20),height=u32(24),levels=u32(40),vkFormat=u32(12);
    if (!width || !height || width>model_core::kMaxTextureDimension || height>model_core::kMaxTextureDimension
        || u32(28)!=0 || u32(32)!=0 || u32(36)!=1 || !levels
        || levels>model_core::FullImageMipCount(width,height) || bytes.size()<80+size_t(levels)*24
        || uint64_t(width)*height>options.maxPixels) return std::nullopt;
    // Metadata ranges are independently bounded before library allocation.
    for (const auto range : {std::pair<uint64_t,uint64_t>{u32(48),u32(52)},
                            std::pair<uint64_t,uint64_t>{u32(56),u32(60)},
                            std::pair<uint64_t,uint64_t>{u64(64),u64(72)}}) {
        if (range.second>1024*1024 || range.first>bytes.size() || range.second>bytes.size()-range.first) return std::nullopt;
    }
    // Only Basis or the closed set of supported Vulkan UNORM/SRGB formats.
    PixelFormatId format=PixelFormatId::Unknown;
    switch (vkFormat) {
    case 0: break;
    case 37: case 43: format=PixelFormatId::RGBA8_UNORM; break;
    case 131: case 132: case 133: case 134: format=PixelFormatId::BC1_UNORM; break;
    case 137: case 138: format=PixelFormatId::BC3_UNORM; break;
    case 141: format=PixelFormatId::BC5_UNORM; break;
    case 145: case 146: format=PixelFormatId::BC7_UNORM; break;
    default: return std::nullopt;
    }
    uint64_t expanded=0;
    for (uint32_t level=0;level<levels;++level) {
        const size_t index=80+size_t(level)*24;
        const uint64_t offset=u64(index),length=u64(index+8),uncompressed=u64(index+16);
        if (offset<80+size_t(levels)*24 || offset>bytes.size() || !length || length>bytes.size()-offset
            || uncompressed>options.maxDecodedBytes || expanded>options.maxDecodedBytes-uncompressed) return std::nullopt;
        expanded+=uncompressed;
        for (uint32_t prior=0;prior<level;++prior) {
            const uint64_t a=u64(80+size_t(prior)*24),b=u64(88+size_t(prior)*24);
            if (offset<a+b && a<offset+length) return std::nullopt;
        }
    }
    const auto worst=model_core::ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,width,height,levels);
    if (!worst || *worst>options.maxDecodedBytes || options.Cancelled()) return std::nullopt;
    ktxTexture2* raw=nullptr;
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
