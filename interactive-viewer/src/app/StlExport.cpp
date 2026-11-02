#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "StlExport.h"

#include "GroundAxis.h"
#include "platform/Win32Handle.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace viewer_stl_export {
namespace {

constexpr std::uint32_t kStlHeaderBytes = 80;
constexpr std::uint32_t kStlFacetBytes = 50;
constexpr std::uint32_t kStlFacetCountOffset = 80;

bool WriteAll(HANDLE file, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size != 0)
    {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, bytes, chunk, &written, nullptr) || written == 0)
            return false;
        bytes += written;
        size -= written;
    }
    return true;
}

void ApplyRowVector(const double matrix[16], double point[3])
{
    const double x = point[0];
    const double y = point[1];
    const double z = point[2];
    point[0] = x * matrix[0] + y * matrix[4] + z * matrix[8] + matrix[12];
    point[1] = x * matrix[1] + y * matrix[5] + z * matrix[9] + matrix[13];
    point[2] = x * matrix[2] + y * matrix[6] + z * matrix[10] + matrix[14];
}

bool ReadVertexPosition(const std::vector<std::byte>& payload, std::size_t vertexBytes,
                        std::size_t stride, std::uint32_t index, const double origin[3],
                        double position[3])
{
    const std::size_t offset = static_cast<std::size_t>(index) * stride;
    if (offset + 12 > vertexBytes) return false;
    float local[3]{};
    std::memcpy(local, payload.data() + offset, sizeof(local));
    for (int axis = 0; axis < 3; ++axis)
        position[axis] = origin[axis] + static_cast<double>(local[axis]);
    return true;
}

bool ReadIndex(const std::vector<std::byte>& payload, std::size_t vertexBytes,
               std::uint32_t index, std::uint32_t& value)
{
    const std::size_t offset = vertexBytes + static_cast<std::size_t>(index) * sizeof(std::uint32_t);
    if (offset + sizeof(std::uint32_t) > payload.size()) return false;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    return true;
}

struct FacetWriter
{
    HANDLE file = nullptr;
    std::uint32_t facetCount = 0;
    std::uint64_t dropped = 0;

    bool WriteFacet(const double a[3], const double b[3], const double c[3])
    {
        const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const double v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const double normal[3] = {
            u[1] * v[2] - u[2] * v[1],
            u[2] * v[0] - u[0] * v[2],
            u[0] * v[1] - u[1] * v[0],
        };
        const double lengthSquared = normal[0] * normal[0] + normal[1] * normal[1]
            + normal[2] * normal[2];
        // Relative degeneracy: compare the cross-product area against the
        // longest edge so a legitimately tiny (unitless) model is not dropped
        // while a collinear sliver still is.
        const double uLengthSquared = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
        const double vLengthSquared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        const double w[3] = { c[0] - b[0], c[1] - b[1], c[2] - b[2] };
        const double wLengthSquared = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
        const double longestEdgeSquared = (std::max)(uLengthSquared,
            (std::max)(vLengthSquared, wLengthSquared));
        if (!std::isfinite(lengthSquared) || !std::isfinite(longestEdgeSquared)
            || longestEdgeSquared <= 0.0 || lengthSquared <= longestEdgeSquared * 1e-12)
        {
            ++dropped;
            return true;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(a[axis]) || !std::isfinite(b[axis]) || !std::isfinite(c[axis]))
            {
                ++dropped;
                return true;
            }
        }
        if (facetCount == (std::numeric_limits<std::uint32_t>::max)())
            return false;

        const double inverseLength = 1.0 / std::sqrt(lengthSquared);
        float record[kStlFacetBytes / sizeof(float)]{};
        record[0] = static_cast<float>(normal[0] * inverseLength);
        record[1] = static_cast<float>(normal[1] * inverseLength);
        record[2] = static_cast<float>(normal[2] * inverseLength);
        const double* vertices[3] = { a, b, c };
        for (int vertex = 0; vertex < 3; ++vertex)
        {
            record[3 + vertex * 3 + 0] = static_cast<float>(vertices[vertex][0]);
            record[3 + vertex * 3 + 1] = static_cast<float>(vertices[vertex][1]);
            record[3 + vertex * 3 + 2] = static_cast<float>(vertices[vertex][2]);
        }
        std::uint16_t attribute = 0;
        if (!WriteAll(file, record, sizeof(record)) || !WriteAll(file, &attribute, sizeof(attribute)))
            return false;
        ++facetCount;
        return true;
    }
};

} // namespace

Result ExportBinaryStl(const d3d12_import_bridge::ImportResult& model,
                       const std::wstring& path,
                       const Options& options)
{
    Result result;
    result.appliedScale = options.metersPerUnit > 0.0 && std::isfinite(options.metersPerUnit)
        ? options.metersPerUnit * 1000.0 : 1.0;

    // The viewer's normal display correction: Automatic resolves the source
    // axis (Y for glTF, Z for STL/PLY) onto the app's Z-up world.
    DirectX::XMFLOAT4X4 upAxisFloat{};
    DirectX::XMStoreFloat4x4(&upAxisFloat,
        GroundAxisTransform(GroundAxis::Automatic, options.sourceUpAxis, false));
    double upAxis[16]{};
    const float* upAxisSource = &upAxisFloat._11;
    for (int element = 0; element < 16; ++element)
        upAxis[element] = static_cast<double>(upAxisSource[element]);

    // Instance occurrences by geometry chunk. A reusable source is emitted only
    // through its instances, exactly like the renderer's draw set.
    std::unordered_map<std::uint32_t, std::vector<const d3d12_import_bridge::ImportedInstance*>>
        instancesByGeometry;
    for (const auto& instance : model.instances)
        instancesByGeometry[instance.data.geometryChunkId].push_back(&instance);

    std::uint64_t triangleChunks = 0;
    for (const auto& mesh : model.meshes)
    {
        if (mesh.topology == model_core::ChunkTopology::PointList
            && mesh.geometry.lodLevel == model_core::kFineLod)
            result.omittedPoints += mesh.vertexCount;
        if (mesh.topology == model_core::ChunkTopology::TriangleList
            && mesh.geometry.lodLevel == model_core::kFineLod)
            ++triangleChunks;
    }
    if (triangleChunks == 0)
    {
        result.error = L"The model has no fine-detail triangle geometry to export.";
        return result;
    }

    platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
    {
        result.error = L"The destination file could not be created.";
        return result;
    }

    auto fail = [&](const wchar_t* text) {
        result.ok = false;
        result.error = text;
        file.reset();
        DeleteFileW(path.c_str());
        return result;
    };

    char header[kStlHeaderBytes]{};
    const char* caption = "Preview 3D binary STL export";
    std::memcpy(header, caption, std::min<std::size_t>(std::strlen(caption), sizeof(header) - 1));
    const std::uint32_t zero = 0;
    if (!WriteAll(file.get(), header, sizeof(header)) || !WriteAll(file.get(), &zero, sizeof(zero)))
        return fail(L"The STL file could not be written.");

    FacetWriter writer;
    writer.file = file.get();

    for (const auto& mesh : model.meshes)
    {
        if (mesh.topology != model_core::ChunkTopology::TriangleList
            || mesh.geometry.lodLevel != model_core::kFineLod)
            continue;

        const std::size_t stride = model_core::VertexStrideForLayout(mesh.vertexLayoutId);
        if (stride < 12) continue;
        const std::size_t vertexBytes = stride * static_cast<std::size_t>(mesh.vertexCount);
        const std::size_t indexBytes = sizeof(std::uint32_t) * static_cast<std::size_t>(mesh.indexCount);
        if (vertexBytes + indexBytes > mesh.payload.size()) continue;

        const auto occurrences = instancesByGeometry.find(mesh.chunkId);
        const bool hasInstances = occurrences != instancesByGeometry.end();
        if (hasInstances && occurrences->second.empty()) continue;

        const std::size_t occurrenceCount = hasInstances ? occurrences->second.size() : 1;
        for (std::size_t occurrence = 0; occurrence < occurrenceCount; ++occurrence)
        {
            const d3d12_import_bridge::ImportedInstance* instance =
                hasInstances ? occurrences->second[occurrence] : nullptr;
            if (instance && !instance->resolvedVisible) continue;

            double world[16]{};
            if (instance)
            {
                std::memcpy(world, instance->worldTransform, sizeof(world));
            }
            else
            {
                world[0] = world[5] = world[10] = world[15] = 1.0;
            }
            const bool mirrored = instance && instance->mirrored;

            for (std::uint32_t index = 0; index + 2 < mesh.indexCount; index += 3)
            {
                std::uint32_t indices[3]{};
                bool valid = true;
                for (int corner = 0; corner < 3 && valid; ++corner)
                    valid = ReadIndex(mesh.payload, vertexBytes, index + static_cast<std::uint32_t>(corner),
                        indices[corner]);
                if (!valid) continue;

                double vertices[3][3]{};
                for (int corner = 0; corner < 3 && valid; ++corner)
                {
                    valid = ReadVertexPosition(mesh.payload, vertexBytes, stride, indices[corner],
                        mesh.geometry.origin, vertices[corner]);
                    if (!valid) break;
                    ApplyRowVector(world, vertices[corner]);
                    ApplyRowVector(upAxis, vertices[corner]);
                    for (int axis = 0; axis < 3; ++axis)
                        vertices[corner][axis] *= result.appliedScale;
                }
                if (!valid) continue;

                const int second = mirrored ? 2 : 1;
                const int third = mirrored ? 1 : 2;
                if (!writer.WriteFacet(vertices[0], vertices[second], vertices[third]))
                    return fail(L"The STL file could not be written.");
            }
        }
    }

    if (writer.facetCount == 0)
        return fail(L"No non-degenerate triangles could be exported.");

    LARGE_INTEGER countOffset{};
    countOffset.QuadPart = kStlFacetCountOffset;
    if (!SetFilePointerEx(file.get(), countOffset, nullptr, FILE_BEGIN)
        || !WriteAll(file.get(), &writer.facetCount, sizeof(writer.facetCount)))
        return fail(L"The STL file could not be finalized.");

    result.ok = true;
    result.facetCount = writer.facetCount;
    result.droppedFacets = writer.dropped;
    return result;
}

} // namespace viewer_stl_export
