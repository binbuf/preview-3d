#define NOMINMAX

// STEP-003 XDE scene adapter. See StepXdeAdapter.h for the contract and
// .docs/stp.md (STEP-003) for the design. No path, directory, URL, registry
// key, or child process is ever opened: every source byte arrives through the
// inherited read-only handle via the product-owned streambuf below.

#include "StepXdeAdapter.h"

#include "model_core/Checksum.h"
#include "model_core/MaterialPayload.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#pragma warning(push, 0)
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <Standard_Version.hxx>
#include <StepData_StepModel.hxx>
#include <TCollection_AsciiString.hxx>
#include <TColStd_SequenceOfAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDocStd_Document.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ColorType.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_XYZ.hxx>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <istream>
#include <limits>
#include <map>
#include <optional>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace step_host {
namespace {

using namespace model_core;

// Chunk identity ranges. Node ids are the small sequential ids the renderer's
// hierarchy expects; geometry/material/instance records use disjoint high
// ranges so every chunkId in the generation is unique and deterministic.
constexpr std::uint32_t kNodeIdBase = 1;
constexpr std::uint32_t kGeometryIdBase = 0x0100'0000;
constexpr std::uint32_t kMaterialIdBase = 0x0200'0000;
constexpr std::uint32_t kInstanceIdBase = 0x0300'0000;
constexpr std::uint32_t kStatusChunkId = 0x0400'0000;

// Product-owned seekable stream over the inherited read-only handle. The host
// has no path; every byte arrives through ReadFile. Mirrors the STEP-001
// proven HandleStreamBuf; kept local so the test-only spike stays independent.
class HandleStreamBuf final : public std::streambuf {
public:
    HandleStreamBuf(HANDLE handle, std::uint64_t size) : handle_(handle), size_(size)
    {
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        return Fill(CurrentOffset()) ? traits_type::to_int_type(*gptr()) : traits_type::eof();
    }

    std::streamsize xsgetn(char* destination, std::streamsize count) override
    {
        std::streamsize copied = 0;
        while (copied < count) {
            if (gptr() < egptr()) {
                const auto available = static_cast<std::streamsize>(egptr() - gptr());
                const auto take = (std::min)(available, count - copied);
                std::memcpy(destination + copied, gptr(), static_cast<std::size_t>(take));
                gbump(static_cast<int>(take));
                copied += take;
                continue;
            }
            if (!Fill(CurrentOffset())) break;
        }
        return copied;
    }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override
    {
        const auto current = static_cast<std::int64_t>(CurrentOffset());
        std::int64_t base = 0;
        if (direction == std::ios_base::cur) base = current;
        else if (direction == std::ios_base::end) base = static_cast<std::int64_t>(size_);
        std::int64_t target = base + offset;
        target = (std::max)(std::int64_t{0}, (std::min)(target, static_cast<std::int64_t>(size_)));
        setg(buffer_.data(), buffer_.data(), buffer_.data());
        bufferStart_ = static_cast<std::uint64_t>(target);
        return pos_type(target);
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
    {
        return seekoff(off_type(position), std::ios_base::beg, mode);
    }

private:
    std::uint64_t CurrentOffset() const
    {
        return bufferStart_ + static_cast<std::uint64_t>(egptr() - buffer_.data());
    }

    bool Fill(std::uint64_t offset)
    {
        if (offset >= size_) return false;
        const auto want = static_cast<DWORD>((std::min)(static_cast<std::uint64_t>(buffer_.size()), size_ - offset));
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) return false;
        DWORD read = 0;
        if (!ReadFile(handle_, buffer_.data(), want, &read, nullptr) || read == 0) return false;
        bufferStart_ = offset;
        setg(buffer_.data(), buffer_.data(), buffer_.data() + read);
        return true;
    }

    HANDLE handle_{};
    std::uint64_t size_{};
    std::uint64_t bufferStart_{};
    std::array<char, 64 * 1024> buffer_{};
};

bool IsCancelled(HANDLE event)
{
    return event && event != INVALID_HANDLE_VALUE && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

// --- exact double affine helpers, matching SharedSectionValidator -------------

bool FiniteAffine(const double matrix[16])
{
    for (std::uint32_t i = 0; i < 16; ++i) {
        if (!std::isfinite(matrix[i]) || std::abs(matrix[i]) > 1e30) return false;
    }
    if (matrix[3] != 0.0 || matrix[7] != 0.0 || matrix[11] != 0.0 || matrix[15] != 1.0) return false;
    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9])
        - matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8])
        + matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return std::isfinite(determinant) && std::abs(determinant) >= 1e-18;
}

bool MultiplyAffine(const double left[16], const double right[16], double out[16])
{
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            double value = 0;
            for (std::uint32_t k = 0; k < 4; ++k) value += left[row * 4 + k] * right[k * 4 + column];
            if (!std::isfinite(value) || std::abs(value) > 1e30) return false;
            out[row * 4 + column] = value;
        }
    }
    return FiniteAffine(out);
}

void IdentityAffine(double out[16])
{
    for (int i = 0; i < 16; ++i) out[i] = 0.0;
    out[0] = out[5] = out[10] = out[15] = 1.0;
}

// gp_Trsf maps p -> L*p + t. The wire contract is row-vector (p * M), so the
// upper-left 3x3 is the transpose of the gp_Trsf linear part and the
// translation occupies elements 12..14.
bool TrsfToAffine(const gp_Trsf& transform, double out[16])
{
    IdentityAffine(out);
    for (std::uint32_t row = 0; row < 3; ++row) {
        for (std::uint32_t column = 0; column < 3; ++column)
            out[row * 4 + column] = transform.Value(static_cast<Standard_Integer>(column + 1),
                                                    static_cast<Standard_Integer>(row + 1));
    }
    const gp_XYZ translation = transform.TranslationPart();
    out[12] = translation.X();
    out[13] = translation.Y();
    out[14] = translation.Z();
    return FiniteAffine(out);
}

// Mirrors SharedSectionValidator's TransformGeometryBounds exactly so the
// broker's independent recomputation agrees to the documented tolerance.
bool TransformBounds(const double origin[3], const float localMin[3], const float localMax[3],
                     const double world[16], double minimum[3], double maximum[3])
{
    minimum[0] = minimum[1] = minimum[2] = (std::numeric_limits<double>::max)();
    maximum[0] = maximum[1] = maximum[2] = -(std::numeric_limits<double>::max)();
    for (std::uint32_t corner = 0; corner < 8; ++corner) {
        const double point[3] = {
            origin[0] + (corner & 1 ? localMax[0] : localMin[0]),
            origin[1] + (corner & 2 ? localMax[1] : localMin[1]),
            origin[2] + (corner & 4 ? localMax[2] : localMin[2]),
        };
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const double value = point[0] * world[axis] + point[1] * world[4 + axis]
                + point[2] * world[8 + axis] + world[12 + axis];
            if (!std::isfinite(value) || std::abs(value) > 1e30) return false;
            minimum[axis] = (std::min)(minimum[axis], value);
            maximum[axis] = (std::max)(maximum[axis], value);
        }
    }
    return true;
}

float SrgbToLinear(float value)
{
    return Quantity_Color::Convert_sRGB_To_LinearRGB((std::max)(0.0f, (std::min)(1.0f, value)));
}

std::uint32_t FloatBits(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct Rgba {
    float r = 0.8f, g = 0.8f, b = 0.8f, a = 1.0f;
    bool operator==(const Rgba& other) const
    {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

Rgba ToRgba(const Quantity_ColorRGBA& color)
{
    Rgba result;
    double red = 0.0, green = 0.0, blue = 0.0;
    color.GetRGB().Values(red, green, blue, Quantity_TOC_sRGB);
    result.r = static_cast<float>(red);
    result.g = static_cast<float>(green);
    result.b = static_cast<float>(blue);
    result.a = static_cast<float>(color.Alpha());
    return result;
}

// Material identity is the exact linear RGBA bit pattern, so a plain uint32
// key cannot carry it. The hash only buckets; the map still compares the full
// four-element key.
using MaterialKey = std::array<std::uint32_t, 4>;
struct MaterialKeyHash {
    std::size_t operator()(const MaterialKey& key) const noexcept
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint32_t word : key) {
            hash ^= word;
            hash *= 1099511628211ull;
        }
        return static_cast<std::size_t>(hash);
    }
};

// --- normalized scene records -------------------------------------------------

using Vertex = VertexPositionNormalUv0F32;

struct GeometryRecord {
    std::uint32_t chunkId = 0;
    std::uint32_t meshId = 0;
    std::vector<Vertex> vertices;
    float localMin[3] = {0, 0, 0};
    float localMax[3] = {0, 0, 0};
};

struct MaterialRecord {
    std::uint32_t chunkId = 0;
    MaterialPayload payload{};
};

struct NodeRecord {
    std::uint32_t nodeId = 0;
    std::uint32_t parentId = 0;
    double transform[16]{};
};

struct InstanceRecord {
    std::uint32_t instanceId = 0;
    std::uint32_t nodeId = 0;
    std::uint32_t geometryId = 0;
    std::uint32_t materialId = 0;
    double worldMin[3]{};
    double worldMax[3]{};
};

struct StepScene {
    std::vector<GeometryRecord> geometry;
    std::vector<MaterialRecord> materials;
    std::vector<NodeRecord> nodes;
    std::vector<InstanceRecord> instances;
    std::uint32_t definitionCount = 0;
    std::uint32_t warningCount = 0;
};

struct SubmeshRef {
    std::uint32_t geometryId = 0;
    std::uint32_t materialId = 0;
};

struct Definition {
    std::uint32_t meshId = 0;
    std::vector<SubmeshRef> groups;       // per definition color group
    std::vector<std::uint32_t> allGeometryIds;
};

// --- builder ------------------------------------------------------------------

class SceneBuilder {
public:
    SceneBuilder(const Handle(TDocStd_Document)& document, const StepXdeLimits& limits)
        : document_(document), limits_(limits)
    {
        shapeTool_ = XCAFDoc_DocumentTool::ShapeTool(document_->Main());
        colorTool_ = XCAFDoc_DocumentTool::ColorTool(document_->Main());
    }

    bool shapeToolValid() const { return !shapeTool_.IsNull(); }

    StepScene Take() { return std::move(scene_); }
    std::uint32_t skippedDefinitions() const { return skippedDefinitions_; }

    model_core::ImportErrorCode Build()
    {
        TDF_LabelSequence roots;
        shapeTool_->GetFreeShapes(roots);
        if (roots.Length() == 0) return model_core::ImportErrorCode::EmptyGeometry;

        for (Standard_Integer index = 1; index <= roots.Length(); ++index) {
            if (cancelled_) return model_core::ImportErrorCode::Cancelled;
            double identity[16];
            IdentityAffine(identity);
            VisitDefinition(roots.Value(index), roots.Value(index), 0, identity, false, 1);
            if (error_ != model_core::ImportErrorCode::None) return error_;
        }
        if (scene_.instances.empty()) return model_core::ImportErrorCode::EmptyGeometry;
        return model_core::ImportErrorCode::None;
    }

    void SetCancellationProbe(std::function<bool()> probe) { cancelledProbe_ = std::move(probe); }

private:
    bool Cancelled()
    {
        if (cancelled_) return true;
        if (cancelledProbe_ && cancelledProbe_()) cancelled_ = true;
        return cancelled_;
    }

    std::uint32_t ResolveMaterial(const Rgba& color)
    {
        const MaterialKey key{
            FloatBits(SrgbToLinear(color.r)), FloatBits(SrgbToLinear(color.g)),
            FloatBits(SrgbToLinear(color.b)), FloatBits((std::max)(0.0f, (std::min)(1.0f, color.a)))};
        const auto existing = materialByKey_.find(key);
        if (existing != materialByKey_.end()) return existing->second;
        if (scene_.materials.size() >= limits_.maxMaterials) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return 0;
        }
        MaterialRecord record;
        record.chunkId = kMaterialIdBase + static_cast<std::uint32_t>(scene_.materials.size());
        record.payload.baseColorFactor[0] = SrgbToLinear(color.r);
        record.payload.baseColorFactor[1] = SrgbToLinear(color.g);
        record.payload.baseColorFactor[2] = SrgbToLinear(color.b);
        const float alpha = (std::max)(0.0f, (std::min)(1.0f, color.a));
        record.payload.baseColorFactor[3] = alpha;
        record.payload.metallicFactor = 0.0f;
        record.payload.roughnessFactor = 0.5f;
        record.payload.emissiveFactor[0] = record.payload.emissiveFactor[1] = record.payload.emissiveFactor[2] = 0.0f;
        record.payload.uvOffset[0] = record.payload.uvOffset[1] = 0.0f;
        record.payload.uvScale[0] = record.payload.uvScale[1] = 1.0f;
        record.payload.uvRotation = 0.0f;
        record.payload.alphaMode = alpha < 0.999f ? static_cast<std::uint32_t>(AlphaModeId::Blend)
                                                  : static_cast<std::uint32_t>(AlphaModeId::Opaque);
        record.payload.alphaCutoff = 0.5f;
        record.payload.flags = 0;
        record.payload.reserved0 = 0;
        materialByKey_.emplace(key, record.chunkId);
        scene_.materials.push_back(record);
        return record.chunkId;
    }

    bool LeafHasColor(const TDF_Label& label, Quantity_ColorRGBA& color) const
    {
        if (colorTool_.IsNull()) return false;
        return colorTool_->GetColor(label, XCAFDoc_ColorSurf, color)
            || colorTool_->GetColor(label, XCAFDoc_ColorGen, color)
            || colorTool_->GetColor(label, XCAFDoc_ColorCurv, color);
    }

    // Builds (once) the reusable geometry for a simple-shape definition. Face
    // appearance follows the documented instance/shape/subshape precedence:
    // the shape-level color (if any) wins; otherwise per-face subshape colors
    // split the geometry into bounded seam groups; otherwise the neutral
    // material (id 0) is used. Instance color overrides are applied later, at
    // occurrence time, without duplicating geometry.
    const Definition* BuildDefinition(const TDF_Label& label)
    {
        TCollection_AsciiString entry;
        TDF_Tool::Entry(label, entry);
        const std::string key(entry.ToCString());
        const auto existing = definitions_.find(key);
        if (existing != definitions_.end()) return &existing->second;
        // A definition that cannot be transferred/tessellated is remembered so
        // a repeated occurrence does not re-mesh or re-warn on every instance.
        if (unsupportedDefinitions_.find(key) != unsupportedDefinitions_.end()) return nullptr;

        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
        if (shape.IsNull()) {
            unsupportedDefinitions_.insert(key);
            ++skippedDefinitions_;
            return nullptr;
        }
        if (scene_.definitionCount >= limits_.maxDefinitions) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return nullptr;
        }

        Definition definition;
        definition.meshId = ++scene_.definitionCount;

        // Per-face subshape colors, only consulted when the shape-level color
        // is absent (shape color wins under the documented precedence).
        Quantity_ColorRGBA shapeColor;
        const bool hasShapeColor = LeafHasColor(label, shapeColor);
        std::vector<std::pair<TopoDS_Shape, std::uint32_t>> faceColors;
        if (!hasShapeColor) CollectFaceColors(label, faceColors);
        if (error_ != model_core::ImportErrorCode::None) {
            unsupportedDefinitions_.insert(key);
            return nullptr;
        }

        MeshIntoGroups(shape, definition, hasShapeColor ? std::optional<Rgba>(ToRgba(shapeColor)) : std::nullopt,
                       faceColors);
        if (error_ != model_core::ImportErrorCode::None) return nullptr;
        if (definition.allGeometryIds.empty()) {
            // A visible definition that cannot be tessellated is not silently
            // published; it is counted and only fatal if nothing else exists.
            --scene_.definitionCount;
            unsupportedDefinitions_.insert(key);
            ++skippedDefinitions_;
            return nullptr;
        }
        const auto [it, inserted] = definitions_.emplace(key, std::move(definition));
        (void)inserted;
        return &it->second;
    }

    void CollectFaceColors(const TDF_Label& label,
                           std::vector<std::pair<TopoDS_Shape, std::uint32_t>>& faceColors)
    {
        if (colorTool_.IsNull() || !shapeTool_) return;
        TDF_LabelSequence subLabels;
        if (!XCAFDoc_ShapeTool::GetSubShapes(label, subLabels)) return;
        for (Standard_Integer index = 1; index <= subLabels.Length(); ++index) {
            if (faceColors.size() >= limits_.maxSubshapes) break;
            const TDF_Label sub = subLabels.Value(index);
            if (!XCAFDoc_ShapeTool::IsSubShape(sub)) continue;
            Quantity_ColorRGBA color;
            if (!colorTool_->GetColor(sub, XCAFDoc_ColorSurf, color)
                && !colorTool_->GetColor(sub, XCAFDoc_ColorGen, color)) {
                continue;
            }
            const TopoDS_Shape subShape = XCAFDoc_ShapeTool::GetShape(sub);
            if (subShape.IsNull() || subShape.ShapeType() != TopAbs_FACE) continue;
            faceColors.emplace_back(subShape, ResolveMaterial(ToRgba(color)));
        }
    }

    void MeshIntoGroups(const TopoDS_Shape& shape, Definition& definition,
                        const std::optional<Rgba>& shapeColor,
                        const std::vector<std::pair<TopoDS_Shape, std::uint32_t>>& faceColors)
    {
        if (error_ != model_core::ImportErrorCode::None) return;
        BRepMesh_IncrementalMesh mesher(shape, limits_.linearDeflection, Standard_True,
                                        limits_.angularDeflection, Standard_True);
        mesher.Perform();

        std::map<std::uint32_t, std::vector<Vertex>> groups;
        for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
            if (Cancelled()) return;
            const TopoDS_Face face = TopoDS::Face(explorer.Current());
            const std::uint32_t materialId = shapeColor
                ? ResolveMaterial(*shapeColor)
                : FaceMaterial(face, faceColors);
            if (error_ != model_core::ImportErrorCode::None) return;

            TopLoc_Location location;
            const Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);
            if (triangulation.IsNull()) continue;
            const bool reversed = face.Orientation() == TopAbs_REVERSED;
            const gp_Trsf& transform = location.Transformation();

            for (Standard_Integer index = 1; index <= triangulation->NbTriangles(); ++index) {
                Standard_Integer n1 = 0, n2 = 0, n3 = 0;
                triangulation->Triangle(index).Get(n1, n2, n3);
                if (reversed) std::swap(n2, n3);
                const gp_Pnt p1 = triangulation->Node(n1).Transformed(transform);
                const gp_Pnt p2 = triangulation->Node(n2).Transformed(transform);
                const gp_Pnt p3 = triangulation->Node(n3).Transformed(transform);
                const double ax = p2.X() - p1.X(), ay = p2.Y() - p1.Y(), az = p2.Z() - p1.Z();
                const double bx = p3.X() - p1.X(), by = p3.Y() - p1.Y(), bz = p3.Z() - p1.Z();
                double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (!(length > 1e-30) || !std::isfinite(length)) continue; // degenerate/invalid
                nx /= length; ny /= length; nz /= length;

                auto& vertices = groups[materialId];
                AppendVertex(vertices, p1, nx, ny, nz);
                AppendVertex(vertices, p2, nx, ny, nz);
                AppendVertex(vertices, p3, nx, ny, nz);

                if (vertices.size() / 3 >= limits_.chunkTriangles) FlushGroup(materialId, vertices, definition);
                if (error_ != model_core::ImportErrorCode::None) return;
            }
        }
        for (auto& [materialId, vertices] : groups) {
            if (error_ != model_core::ImportErrorCode::None) return;
            FlushGroup(materialId, vertices, definition);
        }
    }

    std::uint32_t FaceMaterial(const TopoDS_Face& face,
                               const std::vector<std::pair<TopoDS_Shape, std::uint32_t>>& faceColors) const
    {
        for (const auto& [subShape, materialId] : faceColors) {
            if (subShape.IsSame(face)) return materialId;
        }
        return 0;
    }

    void AppendVertex(std::vector<Vertex>& vertices, const gp_Pnt& point, double nx, double ny, double nz)
    {
        Vertex vertex{};
        vertex.px = static_cast<float>(point.X());
        vertex.py = static_cast<float>(point.Y());
        vertex.pz = static_cast<float>(point.Z());
        vertex.nx = static_cast<float>(nx);
        vertex.ny = static_cast<float>(ny);
        vertex.nz = static_cast<float>(nz);
        vertex.u = 0.0f;
        vertex.v = 0.0f;
        vertices.push_back(vertex);
    }

    void FlushGroup(std::uint32_t materialId, std::vector<Vertex>& vertices, Definition& definition)
    {
        if (vertices.empty()) return;
        takenTriangles_ += static_cast<std::uint64_t>(vertices.size() / 3);
        takenVertices_ += static_cast<std::uint64_t>(vertices.size());
        if (takenTriangles_ > limits_.maxTriangles || takenVertices_ > limits_.maxVertices) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            vertices.clear();
            return;
        }
        GeometryRecord geometry;
        geometry.chunkId = kGeometryIdBase + static_cast<std::uint32_t>(scene_.geometry.size());
        geometry.meshId = definition.meshId;
        geometry.vertices = std::move(vertices);
        vertices.clear();

        const Vertex& first = geometry.vertices.front();
        const float firstPosition[3]{first.px, first.py, first.pz};
        for (int axis = 0; axis < 3; ++axis)
            geometry.localMin[axis] = geometry.localMax[axis] = firstPosition[axis];
        for (const auto& vertex : geometry.vertices) {
            const float position[3]{vertex.px, vertex.py, vertex.pz};
            for (int axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(position[axis])) {
                    error_ = model_core::ImportErrorCode::MalformedData;
                    return;
                }
                geometry.localMin[axis] = (std::min)(geometry.localMin[axis], position[axis]);
                geometry.localMax[axis] = (std::max)(geometry.localMax[axis], position[axis]);
            }
        }
        definition.groups.push_back(SubmeshRef{geometry.chunkId, materialId});
        definition.allGeometryIds.push_back(geometry.chunkId);
        scene_.geometry.push_back(std::move(geometry));
    }

    bool InstanceColor(const TDF_Label& occurrence, Quantity_ColorRGBA& color) const
    {
        if (colorTool_.IsNull()) return false;
        const TopoDS_Shape occurrenceShape = XCAFDoc_ShapeTool::GetShape(occurrence);
        if (occurrenceShape.IsNull()) return false;
        return colorTool_->GetInstanceColor(occurrenceShape, XCAFDoc_ColorGen, color)
            || colorTool_->GetInstanceColor(occurrenceShape, XCAFDoc_ColorSurf, color);
    }

    std::uint32_t AddNode(std::uint32_t parentNodeId, const double transform[16])
    {
        if (scene_.nodes.size() >= limits_.maxNodes) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return 0;
        }
        NodeRecord node;
        node.nodeId = kNodeIdBase + static_cast<std::uint32_t>(scene_.nodes.size());
        node.parentId = parentNodeId;
        std::memcpy(node.transform, transform, sizeof(node.transform));
        scene_.nodes.push_back(node);
        return node.nodeId;
    }

    void AddInstance(std::uint32_t nodeId, const GeometryRecord& geometry, std::uint32_t materialId,
                     const double world[16])
    {
        InstanceRecord instance;
        instance.instanceId = kInstanceIdBase + static_cast<std::uint32_t>(scene_.instances.size());
        instance.nodeId = nodeId;
        instance.geometryId = geometry.chunkId;
        instance.materialId = materialId;
        const double origin[3] = {0.0, 0.0, 0.0};
        if (!TransformBounds(origin, geometry.localMin, geometry.localMax, world,
                             instance.worldMin, instance.worldMax)) {
            error_ = model_core::ImportErrorCode::MalformedData;
            return;
        }
        scene_.instances.push_back(instance);
    }

    void VisitDefinition(const TDF_Label& occurrence, const TDF_Label& definition,
                         std::uint32_t parentNodeId, const double parentWorld[16], bool hasLocation,
                         std::uint32_t depth)
    {
        if (error_ != model_core::ImportErrorCode::None || Cancelled()) return;
        if (depth > limits_.maxHierarchyDepth) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return;
        }

        TCollection_AsciiString entry;
        TDF_Tool::Entry(definition, entry);
        const std::string pathKey(entry.ToCString());
        if (!path_.insert(pathKey).second) {
            error_ = model_core::ImportErrorCode::MalformedData; // cycle
            return;
        }

        double local[16];
        if (hasLocation) {
            if (!TrsfToAffine(XCAFDoc_ShapeTool::GetLocation(occurrence), local)) {
                path_.erase(pathKey);
                error_ = model_core::ImportErrorCode::MalformedData; // singular/non-finite location
                return;
            }
        } else {
            IdentityAffine(local);
        }
        double world[16];
        if (!MultiplyAffine(local, parentWorld, world)) {
            path_.erase(pathKey);
            error_ = model_core::ImportErrorCode::MalformedData;
            return;
        }

        if (XCAFDoc_ShapeTool::IsAssembly(definition)) {
            const std::uint32_t nodeId = AddNode(parentNodeId, local);
            if (error_ != model_core::ImportErrorCode::None) {
                path_.erase(pathKey);
                return;
            }
            TDF_LabelSequence components;
            XCAFDoc_ShapeTool::GetComponents(definition, components);
            for (Standard_Integer index = 1; index <= components.Length(); ++index) {
                if (error_ != model_core::ImportErrorCode::None || Cancelled()) break;
                const TDF_Label component = components.Value(index);
                TDF_Label referred;
                const bool isReference = XCAFDoc_ShapeTool::GetReferredShape(component, referred)
                    && !referred.IsNull();
                const TDF_Label childDefinition = isReference ? referred : component;
                VisitDefinition(component, childDefinition, nodeId, world, true, depth + 1);
            }
            path_.erase(pathKey);
            return;
        }

        if (XCAFDoc_ShapeTool::IsSimpleShape(definition) || XCAFDoc_ShapeTool::IsReference(definition)) {
            const Definition* built = BuildDefinition(definition);
            if (error_ != model_core::ImportErrorCode::None) {
                path_.erase(pathKey);
                return;
            }
            if (!built) {
                path_.erase(pathKey);
                return; // unsupported/unmeshed definition; counted, not drawn
            }

            Quantity_ColorRGBA instanceColor;
            const bool hasInstanceColor = hasLocation && InstanceColor(occurrence, instanceColor);
            const std::uint32_t overrideMaterial = hasInstanceColor ? ResolveMaterial(ToRgba(instanceColor)) : 0;
            if (error_ != model_core::ImportErrorCode::None) {
                path_.erase(pathKey);
                return;
            }

            const std::uint32_t nodeId = AddNode(parentNodeId, local);
            if (error_ != model_core::ImportErrorCode::None) {
                path_.erase(pathKey);
                return;
            }
            if (hasInstanceColor) {
                for (const std::uint32_t geometryId : built->allGeometryIds) {
                    const GeometryRecord& geometry = GeometryById(geometryId);
                    AddInstance(nodeId, geometry, overrideMaterial, world);
                }
            } else {
                for (const SubmeshRef& group : built->groups) {
                    const GeometryRecord& geometry = GeometryById(group.geometryId);
                    AddInstance(nodeId, geometry, group.materialId, world);
                }
            }
            path_.erase(pathKey);
            return;
        }

        path_.erase(pathKey); // neither assembly nor shape: not drawn
    }

    const GeometryRecord& GeometryById(std::uint32_t chunkId) const
    {
        return scene_.geometry[chunkId - kGeometryIdBase];
    }

    Handle(TDocStd_Document) document_;
    Handle(XCAFDoc_ShapeTool) shapeTool_;
    Handle(XCAFDoc_ColorTool) colorTool_;
    StepXdeLimits limits_;
    StepScene scene_;
    std::unordered_map<std::string, Definition> definitions_;
    std::unordered_set<std::string> unsupportedDefinitions_;
    std::unordered_map<MaterialKey, std::uint32_t, MaterialKeyHash> materialByKey_;
    std::unordered_set<std::string> path_;
    std::uint64_t takenTriangles_ = 0;
    std::uint64_t takenVertices_ = 0;
    std::uint32_t skippedDefinitions_ = 0;
    model_core::ImportErrorCode error_ = model_core::ImportErrorCode::None;
    bool cancelled_ = false;
    std::function<bool()> cancelledProbe_;
};

// --- section serialization ----------------------------------------------------

std::optional<std::pair<std::uint32_t, std::uint64_t>> WriteScene(
    std::span<std::byte> destination, const StepScene& scene, std::uint64_t generationId,
    double metersPerUnit, std::uint32_t maxChunkCount)
{
    const bool haveStatus = scene.warningCount != 0;
    const std::uint64_t chunkCount64 = scene.nodes.size() + scene.materials.size()
        + scene.geometry.size() + scene.instances.size() + (haveStatus ? 1 : 0);
    if (chunkCount64 == 0 || chunkCount64 > maxChunkCount
        || chunkCount64 > (std::numeric_limits<std::uint32_t>::max)())
        return std::nullopt;
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(chunkCount64);

    std::uint64_t offset = kSectionHeaderSize
        + static_cast<std::uint64_t>(chunkCount) * kChunkDescriptorSize;
    // Stable addresses: a deque never invalidates the byte buffers already
    // pushed, so the payload spans below stay valid while descriptors are
    // assembled from fixed records and generated geometry.
    std::deque<std::vector<std::byte>> ownedPayloads;
    std::vector<std::pair<ChunkDescriptor, std::span<const std::byte>>> chunks;
    chunks.reserve(chunkCount);

    auto place = [&](const ChunkDescriptor& base, std::span<const std::byte> payload) -> bool {
        ChunkDescriptor descriptor = base;
        descriptor.normalizedRangeOffset = offset;
        descriptor.normalizedRangeLength = payload.size();
        descriptor.byteSize = payload.size();
        descriptor.chunkChecksum = WireChecksum64(payload);
        offset += payload.size();
        if (offset > destination.size()) return false;
        ownedPayloads.emplace_back(payload.begin(), payload.end());
        chunks.emplace_back(descriptor, std::span<const std::byte>(ownedPayloads.back()));
        return true;
    };

    for (const NodeRecord& node : scene.nodes) {
        NodePayload payload{};
        payload.nodeId = node.nodeId;
        payload.parentNodeId = node.parentId;
        payload.flags = kSceneRecordVisible;
        std::memcpy(payload.localTransform, node.transform, sizeof(payload.localTransform));
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Node;
        descriptor.chunkId = node.nodeId;
        if (node.parentId) {
            descriptor.dependencyIds[0] = node.parentId;
            descriptor.dependencyCount = 1;
        }
        if (!place(descriptor, std::as_bytes(std::span(&payload, 1)))) return std::nullopt;
    }
    for (const MaterialRecord& material : scene.materials) {
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Material;
        descriptor.chunkId = material.chunkId;
        if (!place(descriptor, std::as_bytes(std::span(&material.payload, 1)))) return std::nullopt;
    }
    for (const GeometryRecord& geometry : scene.geometry) {
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::TriangleList;
        descriptor.chunkId = geometry.chunkId;
        descriptor.vertexLayoutId = static_cast<std::uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
        descriptor.lodLevel = kFineLod;
        descriptor.meshId = geometry.meshId;
        descriptor.vertexCount = static_cast<std::uint32_t>(geometry.vertices.size());
        descriptor.indexCount = static_cast<std::uint32_t>(geometry.vertices.size());
        // STEP has no decodable source byte range per face, but the broker
        // requires a nonzero range on every geometry chunk; index count is the
        // same convention glTF uses for its source-range disambiguator.
        descriptor.sourceRangeOffset = 0;
        descriptor.sourceRangeLength = descriptor.indexCount;
        descriptor.boundsState = BoundsState::Verified;
        for (int axis = 0; axis < 3; ++axis) {
            descriptor.localMin[axis] = geometry.localMin[axis];
            descriptor.localMax[axis] = geometry.localMax[axis];
        }
        const std::uint64_t vertexBytes = descriptor.vertexCount * sizeof(Vertex);
        const std::uint64_t indexBytes = descriptor.indexCount * sizeof(std::uint32_t);
        std::vector<std::byte> payload(static_cast<std::size_t>(vertexBytes + indexBytes));
        std::memcpy(payload.data(), geometry.vertices.data(), static_cast<std::size_t>(vertexBytes));
        for (std::uint32_t i = 0; i < descriptor.indexCount; ++i) {
            std::memcpy(payload.data() + vertexBytes + static_cast<std::size_t>(i) * sizeof(i), &i, sizeof(i));
        }
        if (!place(descriptor, payload)) return std::nullopt;
    }
    for (const InstanceRecord& instance : scene.instances) {
        MeshInstancePayload payload{};
        payload.instanceId = instance.instanceId;
        payload.nodeId = instance.nodeId;
        payload.geometryChunkId = instance.geometryId;
        payload.materialChunkId = instance.materialId;
        payload.flags = kSceneRecordVisible;
        for (int axis = 0; axis < 3; ++axis) {
            payload.worldMin[axis] = instance.worldMin[axis];
            payload.worldMax[axis] = instance.worldMax[axis];
        }
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::MeshInstance;
        descriptor.chunkId = instance.instanceId;
        descriptor.dependencyIds[0] = instance.geometryId;
        descriptor.dependencyIds[1] = instance.materialId;
        descriptor.dependencyIds[2] = instance.nodeId;
        descriptor.dependencyCount = instance.materialId ? 3u : 2u;
        if (!place(descriptor, std::as_bytes(std::span(&payload, 1)))) return std::nullopt;
    }
    if (haveStatus) {
        ImportStatusPayload status{};
        status.optionalFeatureWarnings = (std::min)(scene.warningCount, 64u);
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = kStatusChunkId;
        if (!place(descriptor, std::as_bytes(std::span(&status, 1)))) return std::nullopt;
    }

    for (std::uint32_t i = 0; i < chunkCount; ++i) {
        std::memcpy(destination.data() + kSectionHeaderSize
                        + static_cast<std::size_t>(i) * kChunkDescriptorSize,
                    &chunks[i].first, sizeof(ChunkDescriptor));
        std::memcpy(destination.data() + chunks[i].first.normalizedRangeOffset,
                    chunks[i].second.data(), chunks[i].second.size());
    }

    SectionHeader header{};
    header.magic = kSectionMagic;
    header.protocolVersion = kCurrentProtocolVersion;
    header.generationId = generationId;
    header.sectionLength = offset;
    header.chunkCount = chunkCount;
    header.scene.generationId = generationId;
    header.scene.format = SourceFormatId::Step;
    header.scene.upAxis = UpAxisId::Unknown;
    header.scene.metersPerUnit = metersPerUnit;
    header.scene.meshCount = scene.definitionCount;
    header.scene.nodeCount = static_cast<std::uint32_t>(scene.nodes.size());
    header.scene.animationCount = 0;
    header.scene.skinCount = 0;
    header.scene.boneCount = 0;
    header.sectionChecksum = WireChecksum64(
        destination.subspan(kSectionHeaderSize, static_cast<std::size_t>(offset - kSectionHeaderSize)));
    std::memcpy(destination.data(), &header, sizeof(header));
    return std::make_pair(chunkCount, offset);
}

} // namespace

StepXdeResult RunStepXdeAdapter(const model_core::ParseStepFileRequest& request,
                                std::span<std::byte> section, HANDLE sourceHandle,
                                HANDLE cancellationEvent, const StepXdeLimits& limits)
{
    StepXdeResult result;
    if (!request.generationId || !request.sourceFileHandleValue || !request.sectionHandleValue
        || request.sectionByteCapacity < sizeof(SectionHeader)
        || section.size() < static_cast<std::size_t>(request.sectionByteCapacity)
        || !request.maxChunkCount) {
        result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        return result;
    }
    if (IsCancelled(cancellationEvent)) {
        result.errorCode = model_core::ImportErrorCode::Cancelled;
        return result;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(sourceHandle, &size) || size.QuadPart <= 0) {
        result.errorCode = model_core::ImportErrorCode::FileUnavailable;
        return result;
    }

    Handle(TDocStd_Document) document;
    try {
        XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", document);
        if (document.IsNull()) {
            result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
            return result;
        }

        STEPCAFControl_Reader reader;
        reader.SetColorMode(Standard_True);
        reader.SetNameMode(Standard_True);
        reader.SetLayerMode(Standard_True);
        reader.SetPropsMode(Standard_False);
        reader.SetGDTMode(Standard_False);
        reader.SetViewMode(Standard_False);

        HandleStreamBuf buffer(sourceHandle, static_cast<std::uint64_t>(size.QuadPart));
        std::istream stream(&buffer);
        if (reader.ReadStream("preview3d.step", stream) != IFSelect_RetDone) {
            result.errorCode = model_core::ImportErrorCode::MalformedData;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }
        if (IsCancelled(cancellationEvent)) {
            result.errorCode = model_core::ImportErrorCode::Cancelled;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }
        if (!reader.Transfer(document)) {
            result.errorCode = model_core::ImportErrorCode::MalformedData;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }

        // The pinned reader normalizes transferred coordinates to the Cascade
        // system unit; GetLengthUnit reports that verified metre factor for the
        // geometry as stored. FileUnits is name-only and is required to prove a
        // length unit was actually authored (absent/contradictory units fail
        // rather than assuming millimetres).
        TColStd_SequenceOfAsciiString lengthUnits, angleUnits, solidAngleUnits;
        reader.ChangeReader().FileUnits(lengthUnits, angleUnits, solidAngleUnits);
        if (lengthUnits.Length() == 0) {
            result.errorCode = model_core::ImportErrorCode::UnsupportedRequiredFeature;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }
        // More than one distinct authored length unit is contradictory under
        // the static-preview contract; do not guess which representation wins.
        for (Standard_Integer index = 2; index <= lengthUnits.Length(); ++index) {
            if (std::strcmp(lengthUnits.Value(index).ToCString(),
                            lengthUnits.Value(1).ToCString()) != 0) {
                result.errorCode = model_core::ImportErrorCode::UnsupportedRequiredFeature;
                XCAFApp_Application::GetApplication()->Close(document);
                return result;
            }
        }
        double metersPerUnit = 0.0;
        if (!XCAFDoc_DocumentTool::GetLengthUnit(document, metersPerUnit)
            || !std::isfinite(metersPerUnit) || metersPerUnit <= 0.0 || metersPerUnit > 1e12) {
            result.errorCode = model_core::ImportErrorCode::UnsupportedRequiredFeature;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }

        SceneBuilder builder(document, limits);
        builder.SetCancellationProbe([cancellationEvent] { return IsCancelled(cancellationEvent); });
        if (!builder.shapeToolValid()) {
            result.errorCode = model_core::ImportErrorCode::EmptyGeometry;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }
        const model_core::ImportErrorCode built = builder.Build();
        if (built != model_core::ImportErrorCode::None) {
            result.errorCode = built;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }
        if (IsCancelled(cancellationEvent)) {
            result.errorCode = model_core::ImportErrorCode::Cancelled;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }

        StepScene scene = builder.Take();
        scene.warningCount = builder.skippedDefinitions();
        const auto written = WriteScene(section, scene, request.generationId, metersPerUnit,
                                        request.maxChunkCount);
        if (!written) {
            result.errorCode = model_core::ImportErrorCode::ResourceLimit;
            XCAFApp_Application::GetApplication()->Close(document);
            return result;
        }
        result.chunkCount = written->first;
        result.sectionBytesWritten = written->second;
        result.definitionCount = scene.definitionCount;
        result.nodeCount = static_cast<std::uint32_t>(scene.nodes.size());
        result.instanceCount = static_cast<std::uint32_t>(scene.instances.size());
        result.materialCount = static_cast<std::uint32_t>(scene.materials.size());
        result.warningCount = scene.warningCount;
        XCAFApp_Application::GetApplication()->Close(document);
        return result;
    } catch (const std::exception&) {
        if (!document.IsNull()) XCAFApp_Application::GetApplication()->Close(document);
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    } catch (...) {
        if (!document.IsNull()) XCAFApp_Application::GetApplication()->Close(document);
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }
}

} // namespace step_host