#define NOMINMAX
#include "WicImageDecodeAdapter.h"
#include "TextureTranscodeAdapter.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "SandboxTestSupport.h"
#include <catch2/catch_test_macros.hpp>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ktx.h>

using Microsoft::WRL::ComPtr;
using namespace model_core;
using namespace import_worker;

namespace {
struct ComScope {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::vector<std::byte> Encode(const CLSID& codec, uint32_t width, uint32_t height, bool pattern=false)
{
    ComPtr<IWICImagingFactory> factory;
    REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))));
    ComPtr<IStream> stream; REQUIRE(SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)));
    ComPtr<IWICBitmapEncoder> encoder;
    REQUIRE(SUCCEEDED(CoCreateInstance(codec,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&encoder))));
    REQUIRE(SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;
    REQUIRE(SUCCEEDED(encoder->CreateNewFrame(&frame,&props)));
    REQUIRE(SUCCEEDED(frame->Initialize(props.Get())));REQUIRE(SUCCEEDED(frame->SetSize(width,height)));
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    REQUIRE(SUCCEEDED(frame->SetPixelFormat(&format)));REQUIRE(format==GUID_WICPixelFormat24bppBGR);
    std::vector<BYTE> pixels(size_t(width)*height*3,128);
    if (pattern) for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
        const BYTE v=(x+y)%2 ? 255 : 0;
        for (unsigned c=0;c<3;++c) pixels[(size_t(y)*width+x)*3+c]=v;
    }
    REQUIRE(SUCCEEDED(frame->WritePixels(height,width*3,static_cast<UINT>(pixels.size()),pixels.data())));
    REQUIRE(SUCCEEDED(frame->Commit()));REQUIRE(SUCCEEDED(encoder->Commit()));
    STATSTG stat{};REQUIRE(SUCCEEDED(stream->Stat(&stat,STATFLAG_NONAME)));
    LARGE_INTEGER zero{};REQUIRE(SUCCEEDED(stream->Seek(zero,STREAM_SEEK_SET,nullptr)));
    std::vector<std::byte> bytes(static_cast<size_t>(stat.cbSize.QuadPart));ULONG read=0;
    REQUIRE(SUCCEEDED(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read)));REQUIRE(read==bytes.size());
    return bytes;
}

struct TempGlb {
    std::filesystem::path path;
    TempGlb(std::span<const std::byte> image, const std::string& slots, const std::string& mime="image/png",unsigned imageIndex=0) {
        path=std::filesystem::temp_directory_path()/(L"Preview3D-texture-"+std::to_wstring(GetCurrentProcessId())+L".glb");
        const std::array<float,9> positions{-1,-1,0,1,-1,0,0,1,0};
        std::vector<std::byte> binary(sizeof(positions));std::memcpy(binary.data(),positions.data(),sizeof(positions));
        binary.insert(binary.end(),image.begin(),image.end());
        const std::string doc="{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":"+std::to_string(image.size())+"}],"
            "\"buffers\":[{\"byteLength\":"+std::to_string(binary.size())+"}],\"images\":[{\"bufferView\":1,\"mimeType\":\""+mime+"\"}],"
            "\"textures\":[{\"source\":"+std::to_string(imageIndex)+"}],\"materials\":["+slots+"]}";
        std::string text=doc;while(text.size()%4)text+=' ';while(binary.size()%4)binary.push_back(std::byte{0});
        const std::array<uint32_t,5> header{0x46546c67,2,static_cast<uint32_t>(28+text.size()+binary.size()),static_cast<uint32_t>(text.size()),0x4e4f534a};
        const std::array<uint32_t,2> bin{static_cast<uint32_t>(binary.size()),0x004e4942};
        std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(header.data()),sizeof(header));
        file.write(text.data(),text.size());file.write(reinterpret_cast<const char*>(bin.data()),sizeof(bin));
        file.write(reinterpret_cast<const char*>(binary.data()),binary.size());REQUIRE(file.good());
    }
    ~TempGlb() { std::error_code error;std::filesystem::remove(path,error); }
    import_broker::ImportSessionRequest Request() const {
        import_broker::ImportSessionRequest request;
        request.workerExePath=sandbox_test_support::WorkerExePath();request.sourcePath=path.wstring();
        request.format=import_broker::ImportFormat::Gltf;request.generationId=203;
        request.maxChunkCount=64;request.maxChunkBatchesPerGeneration=32;request.sectionByteCapacity=1024*1024;return request;
    }
};
}

TEST_CASE("Explicit inbox raster decoders preserve pixels, bound scaling and generate color-aware mips", "[texture-decode]")
{
    ComScope com;
    for (const CLSID* codec : {&CLSID_WICPngEncoder,&CLSID_WICJpegEncoder,&CLSID_WICBmpEncoder,&CLSID_WICTiffEncoder}) {
        auto encoded=Encode(*codec,128,128);
        TextureDecodeOptions options; options.maxDimension=64;
        auto result=DecodeRasterImageWic(encoded,ColorSpaceId::Srgb,options);
        REQUIRE(result);CHECK(result->width==64);CHECK(result->height==64);CHECK(result->mipLevels==7);
        CHECK(result->pixelBytes.size()==*ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,64,64,7));
        for (size_t i=0;i<result->pixelBytes.size();i+=4) {
            CHECK(std::abs(int(std::to_integer<uint8_t>(result->pixelBytes[i]))-128)<=2);
            CHECK(result->pixelBytes[i+3]==std::byte{255});
        }
    }
    auto encoded=Encode(CLSID_WICPngEncoder,3,3,true);
    auto srgb=DecodeRasterImageWic(encoded,ColorSpaceId::Srgb);
    auto linear=DecodeRasterImageWic(encoded,ColorSpaceId::Linear);
    REQUIRE(srgb);REQUIRE(linear);REQUIRE(srgb->mipLevels==2);
    CHECK(std::to_integer<uint8_t>(srgb->pixelBytes[36])==178);
    CHECK(std::to_integer<uint8_t>(linear->pixelBytes[36])==113);
}

TEST_CASE("Raster decode downscales large non-JPEG sources through a bounded scaler", "[texture-decode]")
{
    ComScope com;
    // 3000x3000 = 9,000,000 pixels exceeds the former flat
    // maxDecodedBytes/4 source-pixel guard (8,000,000 here), which used to
    // reject common 4K PNG texture sets outright. The scaler path must still
    // bound the result to maxDimension at full quality.
    auto encoded=Encode(CLSID_WICPngEncoder,3000,3000);
    TextureDecodeOptions options;options.maxDimension=512;
    auto result=DecodeRasterImageWic(encoded,ColorSpaceId::Srgb,options);
    REQUIRE(result);
    CHECK(result->width==375);
    CHECK(result->height==375);
    CHECK(result->pixelBytes.size()==*ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,375,375,result->mipLevels));
    for (size_t i=0;i<result->pixelBytes.size();i+=4)
        CHECK(std::abs(int(std::to_integer<uint8_t>(result->pixelBytes[i]))-128)<=2);
}

TEST_CASE("Raster decode rejects hostile dimensions and expansion, and observes tile cancellation", "[texture-decode]")
{
    ComScope com;auto encoded=Encode(CLSID_WICPngEncoder,256,256,true);
    TextureDecodeOptions options;options.maxPixels=1;CHECK_FALSE(DecodeRasterImageWic(encoded,ColorSpaceId::Srgb,options));
    options={};options.maxDecodedBytes=16;CHECK_FALSE(DecodeRasterImageWic(encoded,ColorSpaceId::Srgb,options));
    options={};options.maxEncodedBytes=encoded.size()-1;CHECK_FALSE(DecodeRasterImageWic(encoded,ColorSpaceId::Srgb,options));
    options={};unsigned checks=0;options.isCancelled=[&]{return ++checks>8;};
    CHECK_FALSE(DecodeRasterImageWic(encoded,ColorSpaceId::Srgb,options));CHECK(checks>8);
    // Forge a valid-CRC IHDR so WIC sees hostile dimensions rather than
    // rejecting only the CRC. No oversized fixture or pixel allocation.
    auto hostile=encoded;
    for (size_t i=16;i<20;++i) hostile[i]=std::byte{255};
    uint32_t crc=0xffffffffu;
    for (size_t i=12;i<29;++i) {
        crc^=std::to_integer<uint8_t>(hostile[i]);
        for (unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1) ? 0xedb88320u : 0);
    }
    crc^=0xffffffffu;
    for (unsigned i=0;i<4;++i) hostile[29+i]=std::byte(uint8_t(crc>>(24-8*i)));
    CHECK_FALSE(DecodeRasterImageWic(hostile,ColorSpaceId::Srgb));
    std::array<std::byte,12> webp{};CHECK_FALSE(DecodeRasterImageWic(webp,ColorSpaceId::Srgb));
    options={};std::vector<std::byte> normals{std::byte{128},std::byte{128},std::byte{255},std::byte{255},
        std::byte{128},std::byte{128},std::byte{255},std::byte{255}};
    options.semantic=TextureSemantic::Normal;uint32_t levels=0;
    REQUIRE(GenerateRasterMips(normals,2,1,ColorSpaceId::Linear,options,levels));CHECK(levels==2);CHECK(normals[10]==std::byte{255});
    options.isCancelled=[] {return true;};CHECK_FALSE(GenerateRasterMips(normals,2,1,ColorSpaceId::Linear,options,levels));
}

TEST_CASE("Sandbox publishes a usable small image before terminal full-chain refinement", "[texture-progressive]")
{
    ComScope com;TempGlb glb(Encode(CLSID_WICPngEncoder,256,256),"{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}");
    auto request=glb.Request();unsigned batches=0,images=0;bool geometry=false;uint32_t root=0;
    request.onBatch=[&](auto chunks) {
        ++batches;
        for (const auto& chunk:chunks) {
            if (chunk.descriptor.topology==ChunkTopology::TriangleList) geometry=true;
            if (chunk.descriptor.topology!=ChunkTopology::Image)continue;
            ++images;ImagePayloadHeader header;std::memcpy(&header,chunk.payload.data(),sizeof(header));
            if (!root) {root=chunk.descriptor.chunkId;CHECK(header.width==64);CHECK(header.reserved0==0);CHECK(header.mipLevels==7);}
            else {CHECK(geometry);CHECK(batches>1);CHECK(header.width==256);CHECK(header.mipLevels==9);CHECK(header.reserved0==root);}
        }
    };
    auto result=import_broker::RunImportSession(request);CAPTURE(result.stage,result.errorCode);REQUIRE(result.ok);CHECK(batches==2);CHECK(images==2);CHECK(geometry);CHECK(result.chunks.empty());
    // Cancellation from accepted low-chain callback prevents the next acknowledgement.
    bool cancelled=false;request.isCancelled=[&]{return cancelled;};request.onBatch=[&](auto){cancelled=true;};
    result=import_broker::RunImportSession(request);CHECK_FALSE(result.ok);CHECK(result.stage==import_broker::ImportStage::Cancelled);
}

TEST_CASE("Shared glTF source images retain separate color, data and normal semantics", "[texture-progressive]")
{
    ComScope com;TempGlb glb(Encode(CLSID_WICPngEncoder,2,2,true),
        "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicRoughnessTexture\":{\"index\":0}},"
        "\"normalTexture\":{\"index\":0},\"emissiveTexture\":{\"index\":0}}");
    auto result=import_broker::RunImportSession(glb.Request());REQUIRE(result.ok);
    unsigned srgb=0,linear=0;
    for (auto& chunk:result.chunks)if (chunk.descriptor.topology==ChunkTopology::Image) {
        ImagePayloadHeader header;std::memcpy(&header,chunk.payload.data(),sizeof(header));
        if (header.colorSpace==uint32_t(ColorSpaceId::Srgb))++srgb;else ++linear;
    }
    CHECK(srgb==2);CHECK(linear==2);
}

TEST_CASE("Corrupt optional texture uses deterministic checker and one bounded warning", "[texture-progressive]")
{
    const std::array<std::byte,4> garbage{};
    TempGlb glb(garbage,"{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}");
    auto result=import_broker::RunImportSession(glb.Request());REQUIRE(result.ok);
    bool geometry=false,checker=false,warning=false;
    for (auto& chunk:result.chunks) {
        geometry|=chunk.descriptor.topology==ChunkTopology::TriangleList;
        if (chunk.descriptor.topology==ChunkTopology::TextureWarning) {uint32_t count;std::memcpy(&count,chunk.payload.data(),4);CHECK(count==1);warning=true;}
        if (chunk.descriptor.topology==ChunkTopology::Image) {CHECK(chunk.payload[32]==std::byte{64});CHECK(chunk.payload[36]==std::byte{192});checker=true;}
    }
    CHECK(geometry);CHECK(checker);CHECK(warning);
    ComScope com;
    TempGlb mismatched(Encode(CLSID_WICPngEncoder,2,2),"{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}","image/jpeg");
    result=import_broker::RunImportSession(mismatched.Request());REQUIRE(result.ok);
    CHECK(std::count_if(result.chunks.begin(),result.chunks.end(),[](const auto& chunk){return chunk.descriptor.topology==ChunkTopology::TextureWarning;})==1);
    TempGlb invalidIndex(garbage,"{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}","image/png",UINT32_MAX);
    result=import_broker::RunImportSession(invalidIndex.Request());REQUIRE(result.ok);
    CHECK(std::count_if(result.chunks.begin(),result.chunks.end(),[](const auto& chunk){return chunk.descriptor.topology==ChunkTopology::TextureWarning;})==1);
}

TEST_CASE("KTX2 and Basis preserve all validated mip levels and semantic targets", "[texture-transcode]")
{
    for (bool basis:{false,true}) {
        ktxTextureCreateInfo info{};info.vkFormat=37;info.baseWidth=info.baseHeight=8;info.baseDepth=1;
        info.numDimensions=2;info.numLevels=4;info.numLayers=info.numFaces=1;
        ktxTexture2* texture=nullptr;REQUIRE(ktxTexture2_Create(&info,KTX_TEXTURE_CREATE_ALLOC_STORAGE,&texture)==KTX_SUCCESS);
        struct Guard { ktxTexture2* t;~Guard(){ktxTexture2_Destroy(t);} } guard{texture};
        for (uint32_t level=0;level<4;++level) {
            std::vector<ktx_uint8_t> rgba(size_t(8u>>level)*(8u>>level)*4,128);
            for (size_t i=3;i<rgba.size();i+=4)rgba[i]=255;
            REQUIRE(ktxTexture_SetImageFromMemory(ktxTexture(texture),level,0,0,rgba.data(),rgba.size())==KTX_SUCCESS);
        }
        if (basis) REQUIRE(ktxTexture2_CompressBasis(texture,128)==KTX_SUCCESS);
        ktx_uint8_t* raw=nullptr;ktx_size_t size=0;
        REQUIRE(ktxTexture_WriteToMemory(ktxTexture(texture),&raw,&size)==KTX_SUCCESS);
        std::vector<std::byte> bytes(reinterpret_cast<std::byte*>(raw),reinterpret_cast<std::byte*>(raw)+size);free(raw);
        for (TextureSemantic semantic:{TextureSemantic::Color,TextureSemantic::Data,TextureSemantic::Normal}) {
            TextureDecodeOptions options;options.semantic=semantic;
            auto result=TranscodeKtx2BasisImage(bytes,options);REQUIRE(result);CHECK(result->mipLevels==4);
            CHECK(result->pixelBytes.size()==*ComputeImagePixelBytes(result->pixelFormat,8,8,4));
            if (semantic!=TextureSemantic::Color || !basis) {
                CHECK(result->pixelFormat==PixelFormatId::RGBA8_UNORM);
                for (size_t i=0;i<result->pixelBytes.size();i+=4) CHECK(std::abs(int(std::to_integer<uint8_t>(result->pixelBytes[i]))-128)<=4);
            }
            options.maxDimension=2;result=TranscodeKtx2BasisImage(bytes,options);REQUIRE(result);CHECK(result->width==2);CHECK(result->mipLevels==2);
            options.maxDecodedBytes=16;CHECK_FALSE(TranscodeKtx2BasisImage(bytes,options));
            options={};options.isCancelled=[] {return true;};CHECK_FALSE(TranscodeKtx2BasisImage(bytes,options));
        }
        for (size_t offset:{20u,24u,28u,32u,36u,40u}) {
            auto hostile=bytes;uint32_t bad=UINT32_MAX;std::memcpy(hostile.data()+offset,&bad,4);
            CHECK_FALSE(TranscodeKtx2BasisImage(hostile));
        }
        auto hostile=bytes;uint64_t expansion=UINT64_MAX;std::memcpy(hostile.data()+96,&expansion,8);
        CHECK_FALSE(TranscodeKtx2BasisImage(hostile));
        hostile=bytes;std::memcpy(hostile.data()+104,hostile.data()+80,8);CHECK_FALSE(TranscodeKtx2BasisImage(hostile));
        hostile=bytes;hostile.resize(90);CHECK_FALSE(TranscodeKtx2BasisImage(hostile));
    }
}
