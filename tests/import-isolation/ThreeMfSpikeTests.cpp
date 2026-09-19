#include <catch2/catch_test_macros.hpp>

#include "SandboxTestSupport.h"
#include "../../interactive-viewer/src/app/D3D12ImportBridge.h"
#include "ThreeMfSpikeWorker.h"
#include "ThreeMfDisplayProperties.h"
#include "ThreeMfOpcPreflight.h"
#include "import_broker/SharedSection.h"
#include "import_broker/ImportSession.h"
#include "import_broker/WorkerPool.h"
#include "model_core/ControlProtocol.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "platform/MappedView.h"
#include "platform/Sha256.h"
#include "platform/Win32Handle.h"

#include <Bindings/Cpp/lib3mf_types.hpp>

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef PREVIEW3D_3MF_FIXTURES_DIR
#error "PREVIEW3D_3MF_FIXTURES_DIR must be defined"
#endif

namespace {

std::vector<std::byte> ReadFile(const char* name)
{
    const auto path = std::filesystem::path(PREVIEW3D_3MF_FIXTURES_DIR) / name;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const auto length = input.tellg();
    REQUIRE(length >= 0);
    std::vector<std::byte> bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), length);
    REQUIRE((input.good() || input.eof()));
    return bytes;
}

std::vector<std::byte> DecodeBase64(const char* name)
{
    const auto encoded = ReadFile(name);
    std::vector<std::byte> decoded;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const std::byte raw : encoded) {
        const unsigned char ch = std::to_integer<unsigned char>(raw);
        if (std::isspace(ch)) continue;
        if (ch == '=') break;
        uint32_t value = 0;
        if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
        else if (ch == '+') value = 62;
        else if (ch == '/') value = 63;
        else FAIL("invalid base64 fixture byte");
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(std::byte((accumulator >> bits) & 0xffu));
        }
    }
    return decoded;
}

std::string Hex(std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const uint8_t value = std::to_integer<uint8_t>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

class TemporaryFile {
public:
    explicit TemporaryFile(std::span<const std::byte> bytes, uint64_t prefixBytes = 0)
    {
        wchar_t directory[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, directory) != 0);
        wchar_t path[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(directory, L"p3m", 0, path) != 0);
        path_ = path;
        handle_.reset(CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        REQUIRE(handle_);
        if (prefixBytes != 0) {
            DWORD ignored = 0;
            REQUIRE(DeviceIoControl(handle_.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                    &ignored, nullptr));
            LARGE_INTEGER prefix{};
            prefix.QuadPart = static_cast<LONGLONG>(prefixBytes);
            REQUIRE(SetFilePointerEx(handle_.get(), prefix, nullptr, FILE_BEGIN));
        }
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset,
                                                              size_t{MAXDWORD}));
            DWORD written = 0;
            REQUIRE(WriteFile(handle_.get(), bytes.data() + offset, chunk, &written, nullptr));
            REQUIRE(written == chunk);
            offset += written;
        }
        REQUIRE(FlushFileBuffers(handle_.get()));
        LARGE_INTEGER zero{};
        REQUIRE(SetFilePointerEx(handle_.get(), zero, nullptr, FILE_BEGIN));
    }

    ~TemporaryFile()
    {
        handle_.reset();
        if (!path_.empty()) DeleteFileW(path_.c_str());
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    HANDLE get() const { return handle_.get(); }
    const std::wstring& path() const { return path_; }
    void Close() { handle_.reset(); }

private:
    std::wstring path_;
    platform::Win32Handle handle_;
};

uint32_t Read32(std::span<const std::byte> bytes, size_t offset)
{
    REQUIRE(offset + 4 <= bytes.size());
    return std::to_integer<uint8_t>(bytes[offset])
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 1])) << 8)
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 2])) << 16)
        | (uint32_t(std::to_integer<uint8_t>(bytes[offset + 3])) << 24);
}

void Put32(std::span<std::byte> bytes, size_t offset, uint32_t value)
{
    REQUIRE(offset + 4 <= bytes.size());
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8] = std::byte((value >> shift) & 0xffu);
}

std::vector<std::byte> RelocateZip(std::span<const std::byte> source, uint32_t prefix)
{
    std::vector<std::byte> relocated(source.begin(), source.end());
    size_t eocd = std::string::npos;
    for (size_t offset = relocated.size() - 22; ; --offset) {
        if (Read32(relocated, offset) == 0x06054b50u) {
            eocd = offset;
            break;
        }
        REQUIRE(offset != 0);
    }
    const uint32_t centralOffset = Read32(relocated, eocd + 16);
    REQUIRE(uint64_t(centralOffset) + prefix <= UINT32_MAX);
    size_t cursor = centralOffset;
    while (cursor < eocd) {
        REQUIRE(Read32(relocated, cursor) == 0x02014b50u);
        const uint16_t nameLength = uint16_t(std::to_integer<uint8_t>(relocated[cursor + 28]))
            | uint16_t(uint16_t(std::to_integer<uint8_t>(relocated[cursor + 29])) << 8);
        const uint16_t extraLength = uint16_t(std::to_integer<uint8_t>(relocated[cursor + 30]))
            | uint16_t(uint16_t(std::to_integer<uint8_t>(relocated[cursor + 31])) << 8);
        const uint16_t commentLength = uint16_t(std::to_integer<uint8_t>(relocated[cursor + 32]))
            | uint16_t(uint16_t(std::to_integer<uint8_t>(relocated[cursor + 33])) << 8);
        const uint32_t localOffset = Read32(relocated, cursor + 42);
        REQUIRE(uint64_t(localOffset) + prefix <= UINT32_MAX);
        Put32(relocated, cursor + 42, localOffset + prefix);
        cursor += 46u + nameLength + extraLength + commentLength;
    }
    REQUIRE(cursor == eocd);
    Put32(relocated, eocd + 16, centralOffset + prefix);
    return relocated;
}

void Append16(std::vector<std::byte>& output, uint16_t value)
{
    output.push_back(std::byte(value & 0xffu));
    output.push_back(std::byte(value >> 8));
}

void Append32(std::vector<std::byte>& output, uint32_t value)
{
    Append16(output, static_cast<uint16_t>(value & 0xffffu));
    Append16(output, static_cast<uint16_t>(value >> 16));
}

uint32_t Crc32(std::span<const std::byte> bytes)
{
    uint32_t crc = 0xffffffffu;
    for (const std::byte byte : bytes) {
        crc ^= std::to_integer<uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::vector<std::byte> BuildStoredPackage(
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    struct CentralEntry {
        uint32_t offset;
        uint32_t crc;
        uint32_t size;
        std::string name;
    };
    std::vector<std::byte> output;
    std::vector<CentralEntry> central;
    for (const auto& [name, content] : entries) {
        REQUIRE(name.size() <= UINT16_MAX);
        REQUIRE(content.size() <= UINT32_MAX);
        const auto contentBytes = std::as_bytes(std::span(content));
        const uint32_t crc = Crc32(contentBytes);
        central.push_back({ static_cast<uint32_t>(output.size()), crc,
                            static_cast<uint32_t>(content.size()), name });
        Append32(output, 0x04034b50u); Append16(output, 20); Append16(output, 0);
        Append16(output, 0); Append16(output, 0); Append16(output, 0); Append32(output, crc);
        Append32(output, static_cast<uint32_t>(content.size()));
        Append32(output, static_cast<uint32_t>(content.size()));
        Append16(output, static_cast<uint16_t>(name.size())); Append16(output, 0);
        output.insert(output.end(), reinterpret_cast<const std::byte*>(name.data()),
                      reinterpret_cast<const std::byte*>(name.data() + name.size()));
        output.insert(output.end(), contentBytes.begin(), contentBytes.end());
    }
    const uint32_t centralOffset = static_cast<uint32_t>(output.size());
    for (const auto& entry : central) {
        Append32(output, 0x02014b50u); Append16(output, 20); Append16(output, 20);
        Append16(output, 0); Append16(output, 0); Append16(output, 0); Append16(output, 0);
        Append32(output, entry.crc); Append32(output, entry.size); Append32(output, entry.size);
        Append16(output, static_cast<uint16_t>(entry.name.size())); Append16(output, 0);
        Append16(output, 0); Append16(output, 0); Append16(output, 0); Append32(output, 0);
        Append32(output, entry.offset);
        output.insert(output.end(), reinterpret_cast<const std::byte*>(entry.name.data()),
                      reinterpret_cast<const std::byte*>(entry.name.data() + entry.name.size()));
    }
    const uint32_t centralSize = static_cast<uint32_t>(output.size()) - centralOffset;
    Append32(output, 0x06054b50u); Append16(output, 0); Append16(output, 0);
    Append16(output, static_cast<uint16_t>(central.size()));
    Append16(output, static_cast<uint16_t>(central.size()));
    Append32(output, centralSize); Append32(output, centralOffset); Append16(output, 0);
    return output;
}

std::vector<std::byte> VertexPressurePackage(size_t extraVertexCount)
{
    std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="1" y="1" z="0"/><vertex x="0" y="1" z="0"/><vertex x="0" y="0" z="1"/><vertex x="1" y="0" z="1"/><vertex x="1" y="1" z="1"/><vertex x="0" y="1" z="1"/>
)";
    model.reserve(extraVertexCount * 32 + 4096);
    for (size_t index = 0; index < extraVertexCount; ++index)
        model += "<vertex x=\"0\" y=\"0\" z=\"0\"/>\n";
    model += R"(</vertices><triangles>
<triangle v1="3" v2="2" v3="1"/><triangle v1="1" v2="0" v3="3"/><triangle v1="4" v2="5" v3="6"/><triangle v1="6" v2="7" v3="4"/><triangle v1="0" v2="1" v3="5"/><triangle v1="5" v2="4" v3="0"/><triangle v1="1" v2="2" v3="6"/><triangle v1="6" v2="5" v3="1"/><triangle v1="2" v2="3" v3="7"/><triangle v1="7" v2="6" v3="2"/><triangle v1="3" v2="0" v3="4"/><triangle v1="4" v2="7" v3="3"/>
</triangles></mesh></object></resources><build><item objectid="1"/></build></model>)";
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    return BuildStoredPackage({ { "[Content_Types].xml", contentTypes },
                                { "_rels/.rels", relationships },
                                { "3D/3dmodel.model", model } });
}

std::vector<std::byte> DeepComponentPackage(uint32_t depth)
{
    REQUIRE(depth >= 2);
    std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources>
<object id="1" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles></mesh></object>
)";
    for (uint32_t id = 2; id <= depth; ++id) {
        model += "<object id=\"" + std::to_string(id)
            + "\" type=\"model\"><components><component objectid=\""
            + std::to_string(id - 1) + "\"/></components></object>\n";
    }
    model += "</resources><build><item objectid=\"" + std::to_string(depth)
        + "\"/></build></model>";
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    return BuildStoredPackage({ { "[Content_Types].xml", contentTypes },
                                { "_rels/.rels", relationships },
                                { "3D/3dmodel.model", model } });
}

std::vector<std::byte> AppearancePackage()
{
    const std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02">
<resources>
<basematerials id="1"><base name="red" displaycolor="#FF0000FF"/><base name="blue" displaycolor="#0000FF80"/></basematerials>
<m:colorgroup id="2"><m:color color="#00FF00FF"/><m:color color="#FFFFFF80"/><m:color color="#0000FFFF"/></m:colorgroup>
<m:compositematerials id="3" matid="1" matindices="0 1"><m:composite values="0.25 0.75"/></m:compositematerials>
<m:multiproperties id="4" pids="1 2" blendmethods="multiply"><m:multi pindices="0 0"/></m:multiproperties>
<m:pbmetallicdisplayproperties id="10"><m:pbmetallic name="brushed" metallicness="0.8" roughness="0.2"/></m:pbmetallicdisplayproperties>
<m:pbspeculardisplayproperties id="11"><m:pbspecular name="polished" specularcolor="#808080" glossiness="0.75"/></m:pbspeculardisplayproperties>
<basematerials id="6" displaypropertiesid="10"><base name="metal" displaycolor="#B0A090FF"/></basematerials>
<basematerials id="7" displaypropertiesid="11"><base name="specular" displaycolor="#606060FF"/></basematerials>
<object id="5" type="model" pid="1" pindex="0"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/>
<vertex x="2" y="0" z="0"/><vertex x="3" y="0" z="0"/><vertex x="2" y="1" z="0"/>
<vertex x="4" y="0" z="0"/><vertex x="5" y="0" z="0"/><vertex x="4" y="1" z="0"/>
<vertex x="6" y="0" z="0"/><vertex x="7" y="0" z="0"/><vertex x="6" y="1" z="0"/>
<vertex x="8" y="0" z="0"/><vertex x="9" y="0" z="0"/><vertex x="8" y="1" z="0"/>
<vertex x="10" y="0" z="0"/><vertex x="11" y="0" z="0"/><vertex x="10" y="1" z="0"/>
</vertices><triangles>
<triangle v1="0" v2="1" v3="2"/>
<triangle v1="3" v2="4" v3="5" pid="2" p1="0" p2="1" p3="2"/>
<triangle v1="6" v2="7" v3="8" pid="3" p1="0"/>
<triangle v1="9" v2="10" v3="11" pid="4" p1="0"/>
<triangle v1="12" v2="13" v3="14" pid="6" p1="0"/>
<triangle v1="15" v2="16" v3="17" pid="7" p1="0"/>
</triangles></mesh></object></resources><build><item objectid="5"/></build></model>)";
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    return BuildStoredPackage({ { "[Content_Types].xml", contentTypes },
                                { "_rels/.rels", relationships },
                                { "3D/3dmodel.model", model } });
}

std::vector<std::byte> DisplayPropertyPackage(std::string_view displayGroup,
                                               std::string_view bases)
{
    std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02">
<resources>)";
    model += displayGroup;
    model += R"(<basematerials id="1" displaypropertiesid="10">)";
    model += bases;
    model += R"(</basematerials>
<object id="2" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/>
</vertices><triangles><triangle v1="0" v2="1" v3="2" pid="1" p1="0"/></triangles></mesh></object>
</resources><build><item objectid="2"/></build></model>)";
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    return BuildStoredPackage({ { "[Content_Types].xml", contentTypes },
                                { "_rels/.rels", relationships },
                                { "3D/3dmodel.model", model } });
}

std::vector<std::byte> LatticePackage(std::string_view resources, uint32_t buildObject)
{
    std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:b="http://schemas.microsoft.com/3dmanufacturing/beamlattice/2017/02"><resources>)";
    model += resources;
    model += "</resources><build><item objectid=\"" + std::to_string(buildObject)
        + "\"/></build></model>";
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    return BuildStoredPackage({ { "[Content_Types].xml", contentTypes },
                                { "_rels/.rels", relationships },
                                { "3D/3dmodel.model", model } });
}

import_broker::ImportSessionResult ImportThreeMfBytes(std::span<const std::byte> bytes,
                                                       uint64_t generation)
{
    TemporaryFile file(bytes);
    file.Close();
    import_broker::ImportSessionRequest request{};
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = file.path();
    request.format = import_broker::ImportFormat::ThreeMf;
    request.generationId = generation;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 2048;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 20'000;
    return import_broker::RunImportSession(request);
}

import_broker::ImportSessionResult ImportThreeMfPath(const std::filesystem::path& path,
                                                      uint64_t generation)
{
    import_broker::ImportSessionRequest request{};
    request.workerExePath = sandbox_test_support::WorkerExePath();
    request.sourcePath = path.wstring();
    request.format = import_broker::ImportFormat::ThreeMf;
    request.generationId = generation;
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 2048;
    request.maxChunkBatchesPerGeneration = 64;
    request.maxChunksPerGeneration = 20'000;
    request.onBatch = [](std::vector<import_broker::ValidatedChunk>&&) {};
    return import_broker::RunImportSession(request);
}

struct Fixture {
    const char* name;
    const char* sha256;
    uint32_t minimumMeshes;
    uint32_t minimumBuildItems;
};

constexpr Fixture kFixtures[] = {
    { "core-box.3mf.base64", "E2633A8C2E014D5F8C612F13F64F48D4E722972C0FB0923795FA83DE5AEE24F6", 1, 1 },
    { "nested-components.3mf.base64", "B5FA98899B990C6BCFB615992D8A21739F6B722A05B4070EC08D591BD277B965", 1, 1 },
    { "production-boxes.3mf.base64", "CC479831E02D01F4696719773B2F4BAEB61BD9F2D3AC3B37CDD9F9FD2078DD97", 2, 2 },
    { "static-production.3mf.base64", "2EFCD56F2D5CD3BB09B66CF902B401A9DD17ADA4396B65658ADA531A4637044E", 2, 2 },
    { "materials-texture.3mf.base64", "52F787357062DA8BFBB14C1B5258A11513717B4BEB072B00AA0161AA76A34B36", 1, 1 },
    { "beam-lattice.3mf.base64", "6814EF817D4845B76717BB33F56E06168758F34A5F0B6AF94DC76F5CDCE0E045", 2, 1 },
    { "beam-representation.3mf.base64", "28248E56B8590EA7E2C33CC375CFA9CDDA89EB98B31AE22EE5072E19D058C8BF", 2, 1 },
};

std::vector<std::byte> CoreBoxVariant(bool unknownRequired, bool privateMetadata)
{
    const auto source = DecodeBase64("core-box.3mf.base64");
    import_worker::ThreeMfOpcPackage package;
    REQUIRE(import_worker::InspectThreeMfOpc(source, &package)
            == import_worker::ThreeMfOpcError::None);
    std::string model;
    for (const auto& part : package.parts) {
        if (part.name != "3d/3dmodel.model") continue;
        std::vector<std::byte> expanded;
        REQUIRE(import_worker::ExtractThreeMfOpcPart(source, part, expanded, 1024 * 1024)
                == import_worker::ThreeMfOpcError::None);
        model.assign(reinterpret_cast<const char*>(expanded.data()), expanded.size());
    }
    REQUIRE_FALSE(model.empty());
    if (unknownRequired) {
        const auto marker = model.find("<model ");
        REQUIRE(marker != std::string::npos);
        model.insert(marker + 7,
            "requiredextensions=\"evil\" xmlns:evil=\"http://example.invalid/3mf/evil\" ");
    }
    const std::string contentTypes = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/><Default Extension="config" ContentType="application/xml"/></Types>)";
    const std::string relationships = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)";
    std::vector<std::pair<std::string, std::string>> entries{
        { "[Content_Types].xml", contentTypes }, { "_rels/.rels", relationships },
        { "3D/3dmodel.model", model },
    };
    if (privateMetadata)
        entries.emplace_back("Metadata/model_settings.config",
            "<config><plate><plater_id>1</plater_id></plate></config>");
    return BuildStoredPackage(entries);
}

std::optional<import_worker::ThreeMfSpikePayloadHeader> RunSpike(
    import_broker::WorkerPool& pool, size_t worker, HANDLE file, uint64_t sourceLength,
    import_worker::ThreeMfSpikeMode mode, uint64_t generation, bool cancelBeforeStart = false,
    std::optional<Lib3MF::eProgressIdentifier> cancelAtPhase = std::nullopt)
{
    const SIZE_T sectionSize = sizeof(import_worker::ThreeMfSpikePayloadHeader);
    auto section = import_broker::CreateSharedSection(sectionSize);
    if (!section) return std::nullopt;
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE, sectionSize);
    if (!view) return std::nullopt;
    auto* header = reinterpret_cast<import_worker::ThreeMfSpikePayloadHeader*>(view.bytes().data());
    std::memset(header, 0, sizeof(*header));
    header->magic = import_worker::kThreeMfSpikePayloadMagic;
    header->sourceByteLength = sourceLength;
    header->mode = static_cast<uint32_t>(mode);
    if (cancelAtPhase)
        header->reserved = static_cast<uint32_t>(*cancelAtPhase) + 1;

    platform::Win32Handle cancellationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancellationEvent) return std::nullopt;
    if (cancelBeforeStart && !SetEvent(cancellationEvent.get())) return std::nullopt;
    const auto workerFile = pool.DuplicateSectionIntoWorker(worker, file);
    const auto workerEvent = pool.DuplicateSectionIntoWorker(worker, cancellationEvent.get());
    const auto workerSection = pool.DuplicateSectionIntoWorker(worker, section.get());
    if (!workerFile || !workerEvent || !workerSection) return std::nullopt;
    header->sourceFileHandleValue = *workerFile;
    header->cancellationEventHandleValue = *workerEvent;

    model_core::StartGenerationRequest request{};
    request.generationId = generation;
    request.sceneVariant = import_worker::kThreeMfSpikeSceneVariant;
    request.sectionHandleValue = *workerSection;
    request.sectionByteCapacity = sectionSize;
    request.maxChunkCount = 1;
    if (!pool.SendRequest(worker, model_core::ControlOpcode::StartGeneration,
                          &request, sizeof(request))) return std::nullopt;
    model_core::ReceivedControlMessage reply{};
    if (pool.WaitForReply(worker, 30'000, reply) != import_broker::WaitReplyOutcome::Ready
        || reply.header.opcode != uint32_t(model_core::ControlOpcode::ChunksReady)) return std::nullopt;
    import_worker::ThreeMfSpikePayloadHeader result{};
    std::memcpy(&result, header, sizeof(result));
    return result;
}

} // namespace

TEST_CASE("3MF-001 pinned lib3mf loads representative packages through an inherited handle",
          "[3mf-spike][callback-io][sandbox][determinism]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    uint64_t generation = 0x336d66010000ull;
    for (const auto& fixture : kFixtures) {
        const auto bytes = DecodeBase64(fixture.name);
        const auto digest = platform::ComputeSha256(bytes);
        REQUIRE(digest);
        CHECK(Hex(*digest) == fixture.sha256);
        uint64_t expectedHash = 0;
        for (unsigned run = 0; run < 3; ++run) {
            TemporaryFile file(bytes);
            const auto result = RunSpike(pool, *worker, file.get(), bytes.size(),
                import_worker::ThreeMfSpikeMode::ReadAndInspect, ++generation);
            CAPTURE(fixture.name, run);
            REQUIRE(result);
            CAPTURE(result->libraryError, result->warningCount,
                    result->progressIdentifierMask, result->shortReadCount,
                    result->sourceChanged);
            REQUIRE(result->status == static_cast<uint32_t>(
                import_worker::ThreeMfSpikeStatus::Success));
            CHECK(result->versionMajor == 2);
            CHECK(result->versionMinor == 5);
            CHECK(result->versionMicro == 0);
            CHECK(result->meshCount >= fixture.minimumMeshes);
            CHECK(result->buildItemCount >= fixture.minimumBuildItems);
            CHECK(result->readCallbackCount > 0);
            CHECK(result->seekCallbackCount > 0);
            CHECK(result->sourceBytesRead > 0);
            CHECK(result->sourceBytesRead < bytes.size() * 8);
            CHECK(result->normalizedBytes > 0);
            if (run == 0) expectedHash = result->normalizedHash;
            else CHECK(result->normalizedHash == expectedHash);
            if (run == 0) {
                std::cout << "3MF-001 fixture=" << fixture.name
                          << " load-us=" << result->loadMicroseconds
                          << " extract-us=" << result->extractMicroseconds
                          << " private-before=" << result->privateBytesBefore
                          << " private-load=" << result->privateBytesAfterLoad
                          << " private-extract=" << result->privateBytesAfterExtract
                          << " peak-working-set=" << result->peakWorkingSetBytes
                          << " callbacks=" << result->readCallbackCount << '/'
                          << result->seekCallbackCount << " hash=0x" << std::hex
                          << result->normalizedHash << std::dec << '\n';
            }
        }
    }
    pool.Release(*worker);
}

TEST_CASE("3MF-001 production and lattice surfaces expose the decisions needed by adapters",
          "[3mf-spike][production][beam-lattice]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const auto productionBytes = DecodeBase64("production-boxes.3mf.base64");
    TemporaryFile productionFile(productionBytes);
    const auto production = RunSpike(pool, *worker, productionFile.get(), productionBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66020001ull);
    REQUIRE(production);
    CAPTURE(production->libraryError, production->warningCount,
            production->progressIdentifierMask, production->shortReadCount,
            production->sourceChanged);
    REQUIRE(production->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(production->buildItemCount == 2);
    CHECK(production->objectCount >= 2);
    CHECK((production->progressIdentifierMask
           & (1u << static_cast<uint32_t>(Lib3MF::eProgressIdentifier::READNONROOTMODELS))) != 0);

    const auto latticeBytes = DecodeBase64("beam-lattice.3mf.base64");
    TemporaryFile latticeFile(latticeBytes);
    const auto lattice = RunSpike(pool, *worker, latticeFile.get(), latticeBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66020002ull);
    REQUIRE(lattice);
    REQUIRE(lattice->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(lattice->beamCount == 12);
    CHECK(lattice->tessellatedTriangleCount > lattice->beamCount * 2);
    CHECK(lattice->tessellatedTriangleCount <= 262'144);
    CHECK(lattice->previewHash != 0);
    CHECK(lattice->instancingEligibleBeamCount == 0);

    const auto representationBytes = DecodeBase64("beam-representation.3mf.base64");
    TemporaryFile representationFile(representationBytes);
    const auto representation = RunSpike(pool, *worker, representationFile.get(),
        representationBytes.size(), import_worker::ThreeMfSpikeMode::ReadAndInspect,
        0x336d66020003ull);
    REQUIRE(representation);
    REQUIRE(representation->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(representation->beamCount > 0);
    CHECK(representation->representationCount == 1);

    const auto deepBytes = DeepComponentPackage(128);
    TemporaryFile deepFile(deepBytes);
    const auto deep = RunSpike(pool, *worker, deepFile.get(), deepBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66020004ull);
    REQUIRE(deep);
    REQUIRE(deep->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(deep->objectCount == 128);
    CHECK(deep->componentObjectCount == 127);
    CHECK(deep->meshCount == 1);
    std::cout << "3MF-001 lattice beams=" << lattice->beamCount
              << " fallback-triangles=" << lattice->tessellatedTriangleCount
              << " instance-eligible=" << lattice->instancingEligibleBeamCount
              << " authored-representations=" << representation->representationCount
              << " deep-components-load-us=" << deep->loadMicroseconds
              << " deep-components-private=" << deep->privateBytesAfterExtract << '\n';
    pool.Release(*worker);
}

TEST_CASE("3MF-001 callback reader uses sparse seeks near the Tier-B source limit",
          "[3mf-spike][callback-io][sparse][limits]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const auto source = DecodeBase64("core-box.3mf.base64");
    const uint64_t targetLength = 2ull * 1024 * 1024 * 1024 - 64 * 1024;
    REQUIRE(targetLength > source.size());
    const uint32_t prefix = static_cast<uint32_t>(targetLength - source.size());
    const auto relocated = RelocateZip(source, prefix);
    TemporaryFile sparseFile(relocated, prefix);
    const auto result = RunSpike(pool, *worker, sparseFile.get(), targetLength,
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66025001ull);
    REQUIRE(result);
    REQUIRE(result->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(result->sparseSeekCount > 0);
    CHECK(result->sourceBytesRead < 1024 * 1024);
    CHECK(result->sourceBytesRead < targetLength / 1024);
    pool.Release(*worker);
}

TEST_CASE("3MF-001 progress cancellation and callback I/O failures are typed and recoverable",
          "[3mf-spike][cancellation][io][recovery]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD processId = pool.ProcessId(*worker);
    const auto bytes = DecodeBase64("materials-texture.3mf.base64");

    TemporaryFile cancelledFile(bytes);
    const auto cancelled = RunSpike(pool, *worker, cancelledFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::CancelFromProgress, 0x336d66030001ull, true);
    REQUIRE(cancelled);
    CHECK(cancelled->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Cancelled));
    CHECK(cancelled->libraryError == LIB3MF_ERROR_CALCULATIONABORTED);
    CHECK(cancelled->progressCallbackCount > 0);

    struct PhaseCase {
        const char* fixture;
        Lib3MF::eProgressIdentifier phase;
    };
    constexpr PhaseCase phaseCases[] = {
        { "core-box.3mf.base64", Lib3MF::eProgressIdentifier::EXTRACTOPCPACKAGE },
        { "core-box.3mf.base64", Lib3MF::eProgressIdentifier::READROOTMODEL },
        { "core-box.3mf.base64", Lib3MF::eProgressIdentifier::READRESOURCES },
        { "production-boxes.3mf.base64", Lib3MF::eProgressIdentifier::READNONROOTMODELS },
        { "materials-texture.3mf.base64", Lib3MF::eProgressIdentifier::READTEXTURETACHMENTS },
    };
    uint64_t phaseGeneration = 0x336d66031000ull;
    for (const auto& phaseCase : phaseCases) {
        const auto phaseBytes = DecodeBase64(phaseCase.fixture);
        TemporaryFile phaseFile(phaseBytes);
        const auto phaseResult = RunSpike(pool, *worker, phaseFile.get(), phaseBytes.size(),
            import_worker::ThreeMfSpikeMode::CancelFromProgress, ++phaseGeneration, false,
            phaseCase.phase);
        CAPTURE(phaseCase.fixture, static_cast<uint32_t>(phaseCase.phase));
        REQUIRE(phaseResult);
        CHECK(phaseResult->status == static_cast<uint32_t>(
            import_worker::ThreeMfSpikeStatus::Cancelled));
        CHECK(phaseResult->libraryError == LIB3MF_ERROR_CALCULATIONABORTED);
    }

    const auto extractionBytes = VertexPressurePackage(250'000);
    TemporaryFile extractionFile(extractionBytes);
    const auto extractionCancelled = RunSpike(pool, *worker, extractionFile.get(),
        extractionBytes.size(), import_worker::ThreeMfSpikeMode::CancelDuringExtraction,
        0x336d66032000ull);
    REQUIRE(extractionCancelled);
    CHECK(extractionCancelled->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Cancelled));
    CHECK(extractionCancelled->extractMicroseconds > 0);

    const auto latticeBytes = DecodeBase64("beam-lattice.3mf.base64");
    TemporaryFile latticeCancellationFile(latticeBytes);
    const auto latticeCancelled = RunSpike(pool, *worker, latticeCancellationFile.get(),
        latticeBytes.size(), import_worker::ThreeMfSpikeMode::CancelDuringLattice,
        0x336d66032001ull);
    REQUIRE(latticeCancelled);
    CHECK(latticeCancelled->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Cancelled));
    CHECK(latticeCancelled->extractMicroseconds > 0);
    std::cout << "3MF-001 extraction-cancel-us="
              << extractionCancelled->extractMicroseconds
              << " lattice-cancel-us=" << latticeCancelled->extractMicroseconds << '\n';

    TemporaryFile shortFile(bytes);
    const auto shortRead = RunSpike(pool, *worker, shortFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::ForceShortRead, 0x336d66030002ull);
    REQUIRE(shortRead);
    CHECK(shortRead->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::IoFailure));
    CHECK(shortRead->shortReadCount > 0);

    TemporaryFile changedFile(bytes);
    const auto sourceChanged = RunSpike(pool, *worker, changedFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::SimulateSourceChange, 0x336d660300025ull);
    REQUIRE(sourceChanged);
    CHECK(sourceChanged->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::IoFailure));
    CHECK(sourceChanged->sourceChanged == 1);
    CHECK(sourceChanged->shortReadCount > 0);

    auto malformedBytes = bytes;
    malformedBytes.resize(malformedBytes.size() / 2);
    TemporaryFile malformedFile(malformedBytes);
    const auto malformed = RunSpike(pool, *worker, malformedFile.get(), malformedBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66030003ull);
    REQUIRE(malformed);
    CHECK(malformed->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Malformed));

    TemporaryFile validFile(bytes);
    const auto recovered = RunSpike(pool, *worker, validFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66030004ull);
    REQUIRE(recovered);
    CHECK(recovered->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    CHECK(pool.ProcessId(*worker) == processId);
    pool.Release(*worker);
}

TEST_CASE("3MF-001 Job commit limit contains model construction pressure and recovers",
          "[3mf-spike][memory][job][recovery]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    import_broker::SandboxLimits limits{};
    limits.processMemoryLimitBytes = 20u * 1024u * 1024u;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), limits, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD originalProcessId = pool.ProcessId(*worker);
    const auto pressureBytes = VertexPressurePackage(1'500'000);
    REQUIRE(pressureBytes.size() > 32u * 1024u * 1024u);
    TemporaryFile pressureFile(pressureBytes);
    const auto exhausted = RunSpike(pool, *worker, pressureFile.get(), pressureBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66035001ull);
    if (exhausted) {
        CAPTURE(exhausted->status, exhausted->libraryError,
                exhausted->privateBytesBefore, exhausted->privateBytesAfterLoad,
                exhausted->peakWorkingSetBytes);
        std::cout << "3MF-001 capped-pressure status=" << exhausted->status
                  << " lib-error=" << exhausted->libraryError
                  << " private-before=" << exhausted->privateBytesBefore
                  << " private-load=" << exhausted->privateBytesAfterLoad
                  << " peak-working-set=" << exhausted->peakWorkingSetBytes << '\n';
        CHECK(exhausted->status != static_cast<uint32_t>(
            import_worker::ThreeMfSpikeStatus::Success));
        CHECK(pool.ProcessId(*worker) == originalProcessId);
    }
    // A library-internal allocation failure is surfaced only as generic error
    // 5 and can retain enough heap state to crowd the deliberately tiny Job.
    // The product boundary therefore retires this worker after pressure.
    REQUIRE(pool.TerminateAndReplace(*worker, error));
    CHECK(pool.ProcessId(*worker) != originalProcessId);

    const auto validBytes = DecodeBase64("core-box.3mf.base64");
    TemporaryFile validFile(validBytes);
    const auto recovered = RunSpike(pool, *worker, validFile.get(), validBytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66035002ull);
    REQUIRE(recovered);
    CHECK(recovered->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    pool.Release(*worker);
}

TEST_CASE("3MF-001 noncooperative post-load work is replaced after the cancellation grace",
          "[3mf-spike][cancellation][worker-pool]")
{
    sandbox_test_support::SandboxFixture sandbox;
    import_broker::WorkerPool pool;
    std::wstring error;
    REQUIRE(pool.InitializeForTesting(sandbox_test_support::WorkerExePath(),
                                      std::move(sandbox.sid), {}, 1,
                                      L"--3mf-spike-pool", error));
    const auto worker = pool.AcquireIdle();
    REQUIRE(worker);
    const DWORD originalProcessId = pool.ProcessId(*worker);
    const auto bytes = DecodeBase64("core-box.3mf.base64");
    TemporaryFile file(bytes);

    const SIZE_T sectionSize = sizeof(import_worker::ThreeMfSpikePayloadHeader);
    auto section = import_broker::CreateSharedSection(sectionSize);
    REQUIRE(section);
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE, sectionSize);
    REQUIRE(view);
    auto* header = reinterpret_cast<import_worker::ThreeMfSpikePayloadHeader*>(view.bytes().data());
    std::memset(header, 0, sizeof(*header));
    header->magic = import_worker::kThreeMfSpikePayloadMagic;
    header->sourceByteLength = bytes.size();
    header->mode = static_cast<uint32_t>(import_worker::ThreeMfSpikeMode::HoldAfterLoad);
    platform::Win32Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    REQUIRE(event);
    const auto workerFile = pool.DuplicateSectionIntoWorker(*worker, file.get());
    const auto workerEvent = pool.DuplicateSectionIntoWorker(*worker, event.get());
    const auto workerSection = pool.DuplicateSectionIntoWorker(*worker, section.get());
    REQUIRE(workerFile); REQUIRE(workerEvent); REQUIRE(workerSection);
    header->sourceFileHandleValue = *workerFile;
    header->cancellationEventHandleValue = *workerEvent;
    model_core::StartGenerationRequest request{};
    request.generationId = 0x336d66040001ull;
    request.sceneVariant = import_worker::kThreeMfSpikeSceneVariant;
    request.sectionHandleValue = *workerSection;
    request.sectionByteCapacity = sectionSize;
    request.maxChunkCount = 1;
    REQUIRE(pool.SendRequest(*worker, model_core::ControlOpcode::StartGeneration,
                             &request, sizeof(request)));
    const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (InterlockedCompareExchange(&header->state, 0, 0) != 2
           && std::chrono::steady_clock::now() < enteredDeadline) Sleep(1);
    REQUIRE(InterlockedCompareExchange(&header->state, 0, 0) == 2);
    const auto cancelStart = std::chrono::steady_clock::now();
    REQUIRE(SetEvent(event.get()));
    model_core::ReceivedControlMessage reply{};
    CHECK(pool.WaitForReply(*worker, 500, reply) == import_broker::WaitReplyOutcome::TimedOut);
    REQUIRE(pool.TerminateAndReplace(*worker, error));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart);
    CHECK(elapsed >= std::chrono::milliseconds(450));
    CHECK(elapsed < std::chrono::milliseconds(2500));
    CHECK(pool.ProcessId(*worker) != originalProcessId);
    std::cout << "3MF-001 noncooperative-replacement-ms=" << elapsed.count() << '\n';
    pool.Release(*worker);

    const auto replacement = pool.AcquireIdle();
    REQUIRE(replacement);
    TemporaryFile validFile(bytes);
    const auto recovered = RunSpike(pool, *replacement, validFile.get(), bytes.size(),
        import_worker::ThreeMfSpikeMode::ReadAndInspect, 0x336d66040002ull);
    REQUIRE(recovered);
    CHECK(recovered->status == static_cast<uint32_t>(
        import_worker::ThreeMfSpikeStatus::Success));
    pool.Release(*replacement);
}

TEST_CASE("3MF-003 normalizes only root-build Core and Production occurrences", "[3mf-003][scene][production]")
{
    struct Expected { const char* fixture; uint32_t meshes; uint32_t occurrences; };
    constexpr Expected fixtures[] = {
        { "core-box.3mf.base64", 1, 1 },
        { "nested-components.3mf.base64", 1, 1 },
        { "static-production.3mf.base64", 2, 2 },
    };
    uint64_t generation = 0x336d66050000ull;
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.fixture);
        const auto bytes = DecodeBase64(fixture.fixture);
        TemporaryFile file(bytes);
        file.Close(); // The trusted broker deliberately takes an exclusive-write source lease.
        import_broker::ImportSessionRequest request{};
        request.workerExePath = sandbox_test_support::WorkerExePath();
        request.sourcePath = file.path();
        request.format = import_broker::ImportFormat::ThreeMf;
        request.generationId = ++generation;
        request.sectionByteCapacity = import_broker::kImportSectionBytes;
        request.maxChunkCount = 1024;
        request.maxChunkBatchesPerGeneration = 64;
        request.maxChunksPerGeneration = 20'000;
        const auto result = import_broker::RunImportSession(request);
        REQUIRE(result.ok);
        REQUIRE_FALSE(result.chunks.empty());
        CHECK(result.chunks.front().scene.format == model_core::SourceFormatId::ThreeMf);
        CHECK(result.chunks.front().scene.upAxis == model_core::UpAxisId::Z);
        CHECK(result.chunks.front().scene.metersPerUnit > 0.0);
        const auto count = [&](model_core::ChunkTopology topology) {
            return std::count_if(result.chunks.begin(), result.chunks.end(), [&](const auto& chunk) {
                return chunk.descriptor.topology == topology;
            });
        };
        CHECK(count(model_core::ChunkTopology::TriangleList) >= fixture.meshes);
        CHECK(count(model_core::ChunkTopology::Node) == fixture.occurrences);
        CHECK(count(model_core::ChunkTopology::MeshInstance) >= fixture.occurrences);
        for (const auto& chunk : result.chunks)
            if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList)
                CHECK((chunk.descriptor.geometryFlags & model_core::kGeometryReusableInstanceSource) != 0);
    }
}

TEST_CASE("3MF-006 shipping viewer bridge routes 3MF without Tier-A request flags",
          "[3mf-006][viewer-bridge]")
{
    TemporaryFile source(DecodeBase64("static-production.3mf.base64"));
    source.Close();
    const auto result = d3d12_import_bridge::RunImport(
        d3d12_import_bridge::SourceFormat::ThreeMf, source.path(), 6001);
    CAPTURE(result.errorStage, result.errorCode, result.errorSummary, result.errorDetails);
    REQUIRE(result.ok);
    CHECK(result.scene.format == model_core::SourceFormatId::ThreeMf);
    CHECK(result.scene.upAxis == model_core::UpAxisId::Z);
    CHECK(result.instances.size() == 2);
    CHECK_FALSE(result.meshes.empty());
}

TEST_CASE("3MF-004 normalizes object defaults, corner colors, composites, and multi-properties",
          "[3mf-004][materials][properties]")
{
    const auto packageBytes = AppearancePackage();
    import_worker::ThreeMfOpcPackage package;
    REQUIRE(import_worker::InspectThreeMfOpc(packageBytes, &package)
            == import_worker::ThreeMfOpcError::None);
    import_worker::ThreeMfDisplayCatalog displayCatalog;
    REQUIRE(import_worker::ScanThreeMfDisplayProperties(packageBytes, package, displayCatalog)
            == model_core::ImportErrorCode::None);
    CHECK(displayCatalog.associations.size() == 2);
    CHECK(displayCatalog.groups.size() == 2);
    const auto result = ImportThreeMfBytes(packageBytes, 0x336d66060001ull);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode));
    REQUIRE(result.ok);
    const auto count = [&](model_core::ChunkTopology topology) {
        return std::count_if(result.chunks.begin(), result.chunks.end(), [&](const auto& chunk) {
            return chunk.descriptor.topology == topology;
        });
    };
    CHECK(count(model_core::ChunkTopology::Material) >= 2);
    CHECK(count(model_core::ChunkTopology::TriangleList) >= 2);
    CHECK(count(model_core::ChunkTopology::MeshInstance) == count(model_core::ChunkTopology::TriangleList));
    bool foundRed = false, foundGradient = false, foundBlend = false, foundSrgb = false;
    bool foundMetallic = false, foundSpecular = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            model_core::MaterialPayload material{};
            REQUIRE(chunk.payload.size() == sizeof(material));
            std::memcpy(&material, chunk.payload.data(), sizeof(material));
            foundBlend |= material.alphaMode == uint32_t(model_core::AlphaModeId::Blend);
            foundSrgb |= (material.flags & model_core::kMaterialFlagVertexSrgb) != 0;
            foundMetallic |= material.metallicFactor > 0.79f && material.metallicFactor < 0.81f
                && material.roughnessFactor > 0.19f && material.roughnessFactor < 0.21f;
            foundSpecular |= material.metallicFactor > 0.17f && material.metallicFactor < 0.20f
                && material.roughnessFactor > 0.24f && material.roughnessFactor < 0.26f;
        }
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
        CHECK(chunk.descriptor.vertexLayoutId == uint32_t(
            model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32));
        CHECK((chunk.descriptor.geometryFlags & model_core::kGeometryHasColors) != 0);
        const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0TangentColorF32*>(
            chunk.payload.data());
        const size_t vertexCount = chunk.descriptor.vertexCount;
        for (size_t index = 0; index < vertexCount; ++index) {
            foundRed |= vertices[index].r > 0.99f && vertices[index].g < 0.01f;
        }
        if (vertexCount >= 3) {
            for (size_t first = 0; first + 2 < vertexCount; first += 3) {
                const auto& a = vertices[first]; const auto& b = vertices[first + 1];
                const auto& c = vertices[first + 2];
                foundGradient |= a.g > 0.99f && b.r > 0.99f && b.a < 0.51f
                    && c.b > 0.99f;
            }
        }
    }
    CHECK(foundRed);
    CHECK(foundGradient);
    CHECK(foundBlend);
    CHECK(foundSrgb);
    CHECK(foundMetallic);
    CHECK(foundSpecular);
}

TEST_CASE("3MF-004 decodes contained texture groups before textured geometry",
          "[3mf-004][texture][wic]")
{
    const auto result = ImportThreeMfBytes(DecodeBase64("materials-texture.3mf.base64"),
                                            0x336d66060002ull);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode));
    REQUIRE(result.ok);
    size_t imagePosition = SIZE_MAX, materialPosition = SIZE_MAX, geometryPosition = SIZE_MAX;
    bool foundUv = false;
    for (size_t index = 0; index < result.chunks.size(); ++index) {
        const auto& chunk = result.chunks[index];
        if (chunk.descriptor.topology == model_core::ChunkTopology::Image) {
            imagePosition = (std::min)(imagePosition, index);
            model_core::ImagePayloadHeader header{};
            REQUIRE(chunk.payload.size() >= sizeof(header));
            std::memcpy(&header, chunk.payload.data(), sizeof(header));
            CHECK(header.colorSpace == uint32_t(model_core::ColorSpaceId::Srgb));
            CHECK(header.width > 0); CHECK(header.height > 0);
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::Material) {
            materialPosition = (std::min)(materialPosition, index);
            CHECK(chunk.descriptor.dependencyIds[0] != 0);
            model_core::MaterialPayload material{};
            REQUIRE(chunk.payload.size() == sizeof(material));
            std::memcpy(&material, chunk.payload.data(), sizeof(material));
            CHECK((material.flags & model_core::kMaterialFlagFlipV) != 0);
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList) {
            geometryPosition = (std::min)(geometryPosition, index);
            foundUv |= (chunk.descriptor.geometryFlags & model_core::kGeometryHasUv0) != 0;
        }
    }
    REQUIRE(imagePosition != SIZE_MAX); REQUIRE(materialPosition != SIZE_MAX);
    REQUIRE(geometryPosition != SIZE_MAX);
    CHECK(imagePosition < materialPosition);
    CHECK(materialPosition < geometryPosition);
    CHECK(foundUv);
}

TEST_CASE("3MF-004 display-property validation is bounded, typed, and recoverable",
          "[3mf-004][display-properties][validation][recovery]")
{
    const auto malformedNumber = DisplayPropertyPackage(
        R"(<m:pbmetallicdisplayproperties id="10"><m:pbmetallic name="bad" metallicness="nan"/></m:pbmetallicdisplayproperties>)",
        R"(<base name="one" displaycolor="#FFFFFFFF"/>)");
    import_worker::ThreeMfOpcPackage package;
    REQUIRE(import_worker::InspectThreeMfOpc(malformedNumber, &package)
            == import_worker::ThreeMfOpcError::None);
    import_worker::ThreeMfDisplayCatalog catalog;
    CHECK(import_worker::ScanThreeMfDisplayProperties(malformedNumber, package, catalog)
          == model_core::ImportErrorCode::MalformedData);

    const auto valid = AppearancePackage();
    REQUIRE(import_worker::InspectThreeMfOpc(valid, &package)
            == import_worker::ThreeMfOpcError::None);
    CHECK(import_worker::ScanThreeMfDisplayProperties(valid, package, catalog,
        [] { return true; }) == model_core::ImportErrorCode::Cancelled);

    const auto cardinalityMismatch = DisplayPropertyPackage(
        R"(<m:pbmetallicdisplayproperties id="10"><m:pbmetallic name="only"/></m:pbmetallicdisplayproperties>)",
        R"(<base name="one" displaycolor="#FFFFFFFF"/><base name="two" displaycolor="#000000FF"/>)");
    const auto rejected = ImportThreeMfBytes(cardinalityMismatch, 0x336d66060003ull);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.errorCode == model_core::ImportErrorCode::MalformedData);

    const auto recovered = ImportThreeMfBytes(valid, 0x336d66060004ull);
    CHECK(recovered.ok);
}

TEST_CASE("3MF-004 unsupported translucent display properties warn and preserve geometry",
          "[3mf-004][display-properties][warning]")
{
    const auto translucent = DisplayPropertyPackage(
        R"(<m:translucentdisplayproperties id="10"><m:translucent name="glass" attenuation="0.5" refractiveindex="1.5" roughness="0.1"/></m:translucentdisplayproperties>)",
        R"(<base name="glass" displaycolor="#80A0C080"/>)");
    const auto result = ImportThreeMfBytes(translucent, 0x336d66060005ull);
    CAPTURE(uint32_t(result.stage), uint32_t(result.errorCode));
    REQUIRE(result.ok);
    const auto warnings = std::count_if(result.chunks.begin(), result.chunks.end(),
        [](const auto& chunk) {
            return chunk.descriptor.topology == model_core::ChunkTopology::TextureWarning;
        });
    CHECK(warnings == 1);
    CHECK(std::any_of(result.chunks.begin(), result.chunks.end(), [](const auto& chunk) {
        return chunk.descriptor.topology == model_core::ChunkTopology::TriangleList;
    }));
}

TEST_CASE("3MF-005 tessellates tapered beams, cap modes, balls, sets, and properties",
          "[3mf-005][beam-lattice][materials]")
{
    const auto package = LatticePackage(R"(
<basematerials id="1"><base name="red" displaycolor="#FF0000FF"/><base name="blue" displaycolor="#0000FFFF"/></basematerials>
<object id="2" type="model" pid="1" pindex="0"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/>
<vertex x="0" y="10" z="0"/><vertex x="5" y="5" z="5"/>
</vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles>
<b:beamlattice minlength="0.01" radius="0.5" cap="sphere" ballmode="mixed" ballradius="1" pid="1" pindex="0">
<b:beams><b:beam v1="0" v2="1" r1="1" r2="2" p1="0" p2="1" cap1="butt" cap2="hemisphere"/>
<b:beam v1="1" v2="3" cap1="sphere" cap2="sphere"/></b:beams>
<b:balls><b:ball vindex="3" r="1.5" p="1"/></b:balls>
<b:beamsets><b:beamset identifier="main"><b:ref index="0"/><b:ref index="1"/><b:ballref index="0"/></b:beamset></b:beamsets>
</b:beamlattice></mesh></object>)", 2);
    import_worker::ThreeMfOpcPackage opc;
    REQUIRE(import_worker::InspectThreeMfOpc(package, &opc) == import_worker::ThreeMfOpcError::None);
    import_worker::ThreeMfDisplayCatalog catalog;
    REQUIRE(import_worker::ScanThreeMfDisplayProperties(package, opc, catalog)
            == model_core::ImportErrorCode::None);
    REQUIRE(catalog.lattices.size() == 1);
    const auto& lattice = catalog.lattices.begin()->second;
    CHECK(lattice.beams.size() == 2);
    CHECK(lattice.balls.size() == 1);
    CHECK(lattice.beamSetCount == 1);

    const auto first = ImportThreeMfBytes(package, 0x336d66070001ull);
    const auto second = ImportThreeMfBytes(package, 0x336d66070002ull);
    CAPTURE(uint32_t(first.stage), uint32_t(first.errorCode));
    REQUIRE(first.ok); REQUIRE(second.ok);
    uint64_t triangleCount = 0;
    bool red = false, blue = false, gradient = false;
    std::vector<std::byte> firstGeometry, secondGeometry;
    const auto inspect = [&](const auto& result, std::vector<std::byte>& geometry) {
        for (const auto& chunk : result.chunks) {
            if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList) continue;
            triangleCount += chunk.descriptor.indexCount / 3;
            geometry.insert(geometry.end(), chunk.payload.begin(), chunk.payload.end());
            const auto* vertices = reinterpret_cast<const model_core::VertexPositionNormalUv0TangentColorF32*>(
                chunk.payload.data());
            for (uint32_t index = 0; index < chunk.descriptor.vertexCount; ++index) {
                red |= vertices[index].r > 0.99f && vertices[index].b < 0.01f;
                blue |= vertices[index].b > 0.99f && vertices[index].r < 0.01f;
            }
            for (uint32_t index = 0; index + 2 < chunk.descriptor.vertexCount; index += 3) {
                bool triangleRed = false, triangleBlue = false;
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    triangleRed |= vertices[index + corner].r > 0.99f
                        && vertices[index + corner].b < 0.01f;
                    triangleBlue |= vertices[index + corner].b > 0.99f
                        && vertices[index + corner].r < 0.01f;
                }
                gradient |= triangleRed && triangleBlue;
            }
        }
    };
    inspect(first, firstGeometry);
    const uint64_t firstTriangleCount = triangleCount;
    triangleCount = 0;
    inspect(second, secondGeometry);
    CHECK(firstTriangleCount > 100);
    CHECK(triangleCount == firstTriangleCount);
    CHECK(firstGeometry == secondGeometry);
    CHECK(red); CHECK(blue); CHECK(gradient);
}

TEST_CASE("3MF-005 prefers an authored representation mesh and supports box clipping",
          "[3mf-005][beam-lattice][representation][clipping]")
{
    const auto represented = LatticePackage(R"(
<object id="1" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles></mesh></object>
<object id="2" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="100" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles>
<b:beamlattice minlength="0.01" radius="10" representationmesh="1"><b:beams><b:beam v1="0" v2="1"/></b:beams></b:beamlattice></mesh></object>)", 2);
    const auto representationResult = ImportThreeMfBytes(represented, 0x336d66070003ull);
    REQUIRE(representationResult.ok);
    uint64_t representedTriangles = 0;
    for (const auto& chunk : representationResult.chunks)
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList)
            representedTriangles += chunk.descriptor.indexCount / 3;
    CHECK(representedTriangles == 2); // source surface plus the one-triangle authored preview

    const auto clippedPackage = LatticePackage(R"(
<object id="1" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/><vertex x="10" y="10" z="0"/><vertex x="0" y="10" z="0"/>
<vertex x="0" y="0" z="10"/><vertex x="10" y="0" z="10"/><vertex x="10" y="10" z="10"/><vertex x="0" y="10" z="10"/>
</vertices><triangles>
<triangle v1="3" v2="2" v3="1"/><triangle v1="1" v2="0" v3="3"/><triangle v1="4" v2="5" v3="6"/><triangle v1="6" v2="7" v3="4"/>
<triangle v1="0" v2="1" v3="5"/><triangle v1="5" v2="4" v3="0"/><triangle v1="1" v2="2" v3="6"/><triangle v1="6" v2="5" v3="1"/>
<triangle v1="2" v2="3" v3="7"/><triangle v1="7" v2="6" v3="2"/><triangle v1="3" v2="0" v3="4"/><triangle v1="4" v2="7" v3="3"/>
</triangles></mesh></object>
<object id="2" type="model"><mesh><vertices><vertex x="-5" y="5" z="5"/><vertex x="15" y="5" z="5"/><vertex x="0" y="6" z="5"/></vertices>
<triangles><triangle v1="0" v2="1" v3="2"/></triangles>
<b:beamlattice minlength="0.01" radius="2" clippingmode="inside" clippingmesh="1"><b:beams><b:beam v1="0" v2="1"/></b:beams></b:beamlattice>
</mesh></object>)", 2);
    const auto clipped = ImportThreeMfBytes(clippedPackage, 0x336d66070004ull);
    CAPTURE(uint32_t(clipped.stage), uint32_t(clipped.errorCode));
    REQUIRE(clipped.ok);
    bool foundClippedLattice = false;
    for (const auto& chunk : clipped.chunks) {
        if (chunk.descriptor.topology != model_core::ChunkTopology::TriangleList
            || chunk.descriptor.vertexCount <= 36) continue;
        foundClippedLattice = true;
        CHECK(chunk.descriptor.origin[0] + chunk.descriptor.localMin[0] >= -1.0e-5);
        CHECK(chunk.descriptor.origin[1] + chunk.descriptor.localMin[1] >= -1.0e-5);
        CHECK(chunk.descriptor.origin[2] + chunk.descriptor.localMin[2] >= -1.0e-5);
        CHECK(chunk.descriptor.origin[0] + chunk.descriptor.localMax[0] <= 100.00001);
        CHECK(chunk.descriptor.origin[1] + chunk.descriptor.localMax[1] <= 100.00001);
        CHECK(chunk.descriptor.origin[2] + chunk.descriptor.localMax[2] <= 100.00001);
    }
    CHECK(foundClippedLattice);
}

TEST_CASE("3MF-005 rejects unsupported clipping and over-budget compact lattices recoverably",
          "[3mf-005][beam-lattice][limits][recovery]")
{
    const auto outside = ImportThreeMfBytes(DecodeBase64("beam-representation.3mf.base64"),
                                             0x336d66070005ull);
    CHECK_FALSE(outside.ok);
    CHECK(outside.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);

    std::string beams;
    beams.reserve(900'000);
    for (uint32_t index = 0; index < 15'000; ++index)
        beams += "<b:beam v1=\"0\" v2=\"1\"/>";
    std::string resources = R"(<object id="1" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/><vertex x="0" y="1" z="0"/>
</vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles>
<b:beamlattice minlength="0.01" radius="1"><b:beams>)";
    resources += beams;
    resources += "</b:beams></b:beamlattice></mesh></object>";
    const auto pressure = ImportThreeMfBytes(LatticePackage(resources, 1), 0x336d66070006ull);
    CHECK_FALSE(pressure.ok);
    CHECK(pressure.errorCode == model_core::ImportErrorCode::ResourceLimit);

    const auto recovered = ImportThreeMfBytes(AppearancePackage(), 0x336d66070007ull);
    CHECK(recovered.ok);
}

TEST_CASE("3MF-005 validates compact lattice data before tessellation",
          "[3mf-005][beam-lattice][validation][cancellation]")
{
    const auto make = [](std::string_view lattice) {
        std::string resources = R"(<object id="1" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/><vertex x="0" y="1" z="0"/>
</vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles>)";
        resources += lattice;
        resources += "</mesh></object>";
        return LatticePackage(resources, 1);
    };
    const std::string_view invalid[] = {
        R"(<b:beamlattice minlength="0.01" radius="nan"><b:beams><b:beam v1="0" v2="1"/></b:beams></b:beamlattice>)",
        R"(<b:beamlattice minlength="0.01" radius="1"><b:beams><b:beam v1="0" v2="99"/></b:beams></b:beamlattice>)",
        R"(<b:beamlattice minlength="0.01" radius="1"><b:beams><b:beam v1="0" v2="1" r1="0"/></b:beams></b:beamlattice>)",
        R"(<b:beamlattice minlength="0.01" radius="1"><b:beams><b:beam v1="0" v2="1"/></b:beams><b:beamsets><b:beamset identifier="bad"><b:ref index="2"/></b:beamset></b:beamsets></b:beamlattice>)",
    };
    uint64_t generation = 0x336d66070010ull;
    for (const auto xml : invalid) {
        const auto result = ImportThreeMfBytes(make(xml), ++generation);
        CHECK_FALSE(result.ok);
        CHECK(result.errorCode == model_core::ImportErrorCode::MalformedData);
    }

    const auto shortBeam = ImportThreeMfBytes(make(
        R"(<b:beamlattice minlength="2" radius="1"><b:beams><b:beam v1="0" v2="2"/></b:beams></b:beamlattice>)"),
        ++generation);
    REQUIRE(shortBeam.ok);
    uint64_t triangles = 0;
    for (const auto& chunk : shortBeam.chunks)
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList)
            triangles += chunk.descriptor.indexCount / 3;
    CHECK(triangles == 1); // the sub-minimum beam is ignored by specification

    const auto packageBytes = make(
        R"(<b:beamlattice minlength="0.01" radius="1"><b:beams><b:beam v1="0" v2="1"/></b:beams></b:beamlattice>)");
    import_worker::ThreeMfOpcPackage package;
    REQUIRE(import_worker::InspectThreeMfOpc(packageBytes, &package)
            == import_worker::ThreeMfOpcError::None);
    import_worker::ThreeMfDisplayCatalog catalog;
    CHECK(import_worker::ScanThreeMfDisplayProperties(packageBytes, package, catalog,
        [] { return true; }) == model_core::ImportErrorCode::Cancelled);
}

TEST_CASE("3MF-007 malformed OPC inputs and unknown requirements recover in the real worker",
          "[3mf-007][archive][recovery]")
{
    const auto valid = DecodeBase64("core-box.3mf.base64");
    auto brokenLocal = valid;
    REQUIRE(brokenLocal.size() > 4);
    brokenLocal[0] = std::byte{'B'};
    auto brokenCentral = valid;
    const auto end = brokenCentral.size() - 22;
    REQUIRE(Read32(brokenCentral, end) == 0x06054b50u);
    Put32(brokenCentral, end + 16, 0xfffffff0u);
    auto unsafePath = valid;
    size_t central = 0;
    while (central + 49 < unsafePath.size()
           && Read32(unsafePath, central) != 0x02014b50u) ++central;
    REQUIRE(central + 49 < unsafePath.size());
    REQUIRE(unsafePath[central + 46] == std::byte{'3'});
    unsafePath[central + 46] = std::byte{'.'};
    unsafePath[central + 47] = std::byte{'.'};
    unsafePath[central + 48] = std::byte{'/'};

    struct InvalidCase {
        std::vector<std::byte> bytes;
        import_worker::ThreeMfOpcError preflight;
        model_core::ImportErrorCode broker;
    };
    const std::vector<InvalidCase> cases{
        { {}, import_worker::ThreeMfOpcError::NotZip,
          model_core::ImportErrorCode::EmptyGeometry },
        { std::vector<std::byte>(valid.begin(), valid.end() - 11),
          import_worker::ThreeMfOpcError::NotZip, model_core::ImportErrorCode::ArchiveLimit },
        { brokenLocal, import_worker::ThreeMfOpcError::InvalidDirectory,
          model_core::ImportErrorCode::ArchiveLimit },
        { brokenCentral, import_worker::ThreeMfOpcError::InvalidDirectory,
          model_core::ImportErrorCode::ArchiveLimit },
        { unsafePath, import_worker::ThreeMfOpcError::UnsafePath,
          model_core::ImportErrorCode::ArchiveLimit },
    };
    uint64_t generation = 0x336d66080000ull;
    for (const auto& input : cases) {
        CAPTURE(generation, uint32_t(input.preflight));
        CHECK(import_worker::InspectThreeMfOpc(input.bytes) == input.preflight);
        const auto rejected = ImportThreeMfBytes(input.bytes, ++generation);
        CHECK_FALSE(rejected.ok);
        CHECK(rejected.errorCode == input.broker);
        const auto recovered = ImportThreeMfBytes(valid, ++generation);
        CHECK(recovered.ok);
    }

    const auto privateMetadata = ImportThreeMfBytes(CoreBoxVariant(false, true), ++generation);
    CHECK(privateMetadata.ok);
    const auto unknownRequired = ImportThreeMfBytes(CoreBoxVariant(true, false), ++generation);
    CHECK_FALSE(unknownRequired.ok);
    CHECK(unknownRequired.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);
    const auto requiredSlice = ImportThreeMfBytes(
        DecodeBase64("production-boxes.3mf.base64"), ++generation);
    CHECK_FALSE(requiredSlice.ok);
    CHECK(requiredSlice.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature);
    const auto optionalBytes = DecodeBase64("static-production.3mf.base64");
    const auto optionalPreflight = import_worker::InspectThreeMfOpc(optionalBytes);
    CAPTURE(uint32_t(optionalPreflight));
    const auto optionalSlice = ImportThreeMfBytes(optionalBytes, ++generation);
    CHECK(optionalSlice.ok);
    const auto recovered = ImportThreeMfBytes(valid, ++generation);
    CHECK(recovered.ok);
}

TEST_CASE("manually supplied 3MF corpus imports through the production worker",
          "[.manual-3mf]")
{
    constexpr wchar_t variable[] = L"PREVIEW3D_MANUAL_3MF_DIR";
    const DWORD required = GetEnvironmentVariableW(variable, nullptr, 0);
    if (!required) SKIP("set PREVIEW3D_MANUAL_3MF_DIR to a directory of local .3mf files");
    std::wstring directory(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(variable, directory.data(), required);
    REQUIRE(written > 0);
    directory.resize(written);

    uint64_t generation = 0x336d66ff0000ull;
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".3mf") continue;
        ++files;
        const auto bytes = [&] {
            std::ifstream input(entry.path(), std::ios::binary | std::ios::ate);
            REQUIRE(input);
            const auto length = input.tellg();
            REQUIRE(length >= 0);
            std::vector<std::byte> value(static_cast<size_t>(length));
            input.seekg(0);
            if (!value.empty()) input.read(reinterpret_cast<char*>(value.data()), length);
            return value;
        }();
        import_worker::ThreeMfOpcPackage package;
        const auto opc = import_worker::InspectThreeMfOpc(bytes, &package);
        import_worker::ThreeMfDisplayCatalog catalog;
        const auto display = opc == import_worker::ThreeMfOpcError::None
            ? import_worker::ScanThreeMfDisplayProperties(bytes, package, catalog)
            : model_core::ImportErrorCode::ArchiveLimit;
        const auto result = ImportThreeMfPath(entry.path(), ++generation);
        CAPTURE(entry.path().filename().string(), uint32_t(result.stage),
                uint32_t(result.errorCode), result.batchCount, uint32_t(opc),
                uint32_t(display), package.parts.size(), catalog.groups.size());
        CHECK(opc == import_worker::ThreeMfOpcError::None);
        CHECK(display == model_core::ImportErrorCode::None);
        CHECK((result.ok
               || result.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature));
    }
    REQUIRE(files > 0);
}
