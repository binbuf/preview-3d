#include "TextureTranscodeAdapter.h"

#include <ktx.h>

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

} // namespace

std::optional<TranscodedImage> TranscodeKtx2BasisImage(std::span<const std::byte> ktx2Bytes)
{
    ktxTexture2* rawTexture = nullptr;
    KTX_error_code createResult = ktxTexture2_CreateFromMemory(
        reinterpret_cast<const ktx_uint8_t*>(ktx2Bytes.data()), ktx2Bytes.size(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &rawTexture);
    if (createResult != KTX_SUCCESS || rawTexture == nullptr) {
        return std::nullopt;
    }
    KtxTexture2Guard guard(rawTexture);
    ktxTexture2* texture = guard.get();

    if (!ktxTexture2_NeedsTranscoding(texture)) {
        // Not a Basis Universal-encoded KTX2 (e.g. already a supported
        // uncompressed/BC container) -- out of scope this slice, which only
        // handles the KHR_texture_basisu transcode path.
        return std::nullopt;
    }

    PixelFormatId resultFormat = PixelFormatId::BC7_UNORM;
    KTX_error_code transcodeResult = ktxTexture2_TranscodeBasis(texture, KTX_TTF_BC7_RGBA, 0);
    if (transcodeResult != KTX_SUCCESS) {
        resultFormat = PixelFormatId::RGBA8_UNORM;
        transcodeResult = ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, 0);
        if (transcodeResult != KTX_SUCCESS) {
            return std::nullopt;
        }
    }

    if (texture->baseWidth == 0 || texture->baseHeight == 0) {
        return std::nullopt;
    }

    ktx_size_t levelOffset = 0;
    if (ktxTexture_GetImageOffset(ktxTexture(texture), 0, 0, 0, &levelOffset) != KTX_SUCCESS) {
        return std::nullopt;
    }
    ktx_size_t levelSize = ktxTexture_GetImageSize(ktxTexture(texture), 0);
    const ktx_uint8_t* data = ktxTexture_GetData(ktxTexture(texture));
    if (data == nullptr || levelSize == 0) {
        return std::nullopt;
    }

    const auto expectedBytes = model_core::ComputeImagePixelBytes(resultFormat, texture->baseWidth,
                                                                    texture->baseHeight, 1);
    if (!expectedBytes || *expectedBytes != levelSize) {
        // The transcoder's own level size disagrees with this adapter's
        // block-size math -- reject rather than trust either side alone.
        return std::nullopt;
    }

    TranscodedImage result;
    result.pixelFormat = resultFormat;
    result.width = texture->baseWidth;
    result.height = texture->baseHeight;
    const auto* levelBytes = reinterpret_cast<const std::byte*>(data + levelOffset);
    result.pixelBytes.assign(levelBytes, levelBytes + levelSize);
    return result;
}

} // namespace import_worker
