#define NOMINMAX

// STEP-003/004 XDE scene adapter. See StepXdeAdapter.h for the contract and
// .docs/stp.md (STEP-003, STEP-004) for the design. No path, directory, URL,
// registry key, or child process is ever opened: every source byte arrives
// through the inherited read-only handle via the product-owned streambuf below.
//
// STEP-004 structure:
//   * phase A (ScenePlanner) walks the XDE document without meshing and builds
//     the node tree, reusable definitions, resolved materials, and occurrence
//     list;
//   * phase B (SceneEmitter) tessellates one definition at a time under the
//     versioned quality profile, writes cluster-local geometry with double
//     origins, and hands each full output window off through the progressive
//     batch publisher. Vertex data is discarded as soon as it is written, so
//     the host never retains the whole normalized scene to deduplicate it.

#include "StepXdeAdapter.h"

#include "model_core/Checksum.h"
#include "model_core/MaterialPayload.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <windows.h>

#pragma warning(push, 0)
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IMeshTools_Parameters.hxx>
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

// Cancellation is polled between bounded face blocks, never only once per
// definition, so a definition with many faces cannot hide a cancelled
// generation behind a long uninterrupted extraction loop.
constexpr std::uint32_t kDefinitionCancelCheckInterval = 4096;

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

// A geometry chunk already written for a definition, retained only as identity
// and bounds so occurrences can reference it without keeping its vertices.
// Positions become cluster-local floats at emission time; `origin` restores
// the transferred double coordinates exactly as the broker recomputes them.
struct EmittedGeometryRef {
    std::uint32_t chunkId = 0;
    std::uint32_t materialId = 0;
    double origin[3] = {0, 0, 0};
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

struct PlannedOccurrence {
    std::uint32_t nodeId = 0;
    double world[16]{};
    bool hasOverride = false;
    std::uint32_t overrideMaterialId = 0;
};

struct PlannedDefinition {
    std::uint32_t meshId = 0;
    TopoDS_Shape shape;
    bool hasShapeColor = false;
    std::uint32_t shapeMaterialId = 0;
    std::vector<std::pair<TopoDS_Shape, std::uint32_t>> faceColors;
    std::vector<PlannedOccurrence> occurrences;
};

// --- phase A: bounded planning without tessellation --------------------------

class ScenePlanner {
public:
    ScenePlanner(const Handle(TDocStd_Document)& document, const StepXdeLimits& limits)
        : document_(document), limits_(limits)
    {
        shapeTool_ = XCAFDoc_DocumentTool::ShapeTool(document_->Main());
        colorTool_ = XCAFDoc_DocumentTool::ColorTool(document_->Main());
    }

    bool shapeToolValid() const { return !shapeTool_.IsNull(); }

    model_core::ImportErrorCode Build()
    {
        TDF_LabelSequence roots;
        shapeTool_->GetFreeShapes(roots);
        if (roots.Length() == 0) return model_core::ImportErrorCode::EmptyGeometry;

        for (Standard_Integer index = 1; index <= roots.Length(); ++index) {
            if (Cancelled()) return model_core::ImportErrorCode::Cancelled;
            double identity[16];
            IdentityAffine(identity);
            VisitDefinition(roots.Value(index), roots.Value(index), 0, identity, false, 1);
            if (error_ != model_core::ImportErrorCode::None) return error_;
        }
        if (!hasOccurrence_) return model_core::ImportErrorCode::EmptyGeometry;
        return model_core::ImportErrorCode::None;
    }

    void SetCancellationProbe(std::function<bool()> probe) { cancelledProbe_ = std::move(probe); }

    const std::vector<NodeRecord>& nodes() const { return nodes_; }
    const std::vector<MaterialRecord>& materials() const { return materials_; }
    const std::deque<PlannedDefinition>& definitions() const { return definitions_; }
    std::uint32_t definitionCount() const { return static_cast<std::uint32_t>(definitions_.size()); }
    std::uint32_t warningCount() const { return warningCount_; }

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
        if (materials_.size() >= limits_.maxMaterials) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return 0;
        }
        MaterialRecord record;
        record.chunkId = kMaterialIdBase + static_cast<std::uint32_t>(materials_.size());
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
        materials_.push_back(record);
        return record.chunkId;
    }

    bool LeafHasColor(const TDF_Label& label, Quantity_ColorRGBA& color) const
    {
        if (colorTool_.IsNull()) return false;
        return colorTool_->GetColor(label, XCAFDoc_ColorSurf, color)
            || colorTool_->GetColor(label, XCAFDoc_ColorGen, color)
            || colorTool_->GetColor(label, XCAFDoc_ColorCurv, color);
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

    // Plans (once) the reusable geometry identity for a simple-shape definition.
    // Face appearance follows the documented instance/shape/subshape precedence:
    // the shape-level color (if any) wins; otherwise per-face subshape colors
    // split the geometry into bounded seam groups; otherwise the neutral
    // material (id 0) is used. Instance color overrides are applied later, at
    // occurrence time, without duplicating geometry.
    bool PlanDefinition(const TDF_Label& label, PlannedDefinition** out)
    {
        TCollection_AsciiString entry;
        TDF_Tool::Entry(label, entry);
        const std::string key(entry.ToCString());
        const auto existing = definitionsByKey_.find(key);
        if (existing != definitionsByKey_.end()) {
            *out = existing->second;
            return true;
        }
        if (unsupportedDefinitions_.find(key) != unsupportedDefinitions_.end()) {
            *out = nullptr;
            return true;
        }

        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
        if (shape.IsNull()) {
            unsupportedDefinitions_.insert(key);
            ++warningCount_;
            *out = nullptr;
            return true;
        }
        if (definitions_.size() >= limits_.maxDefinitions) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            *out = nullptr;
            return false;
        }

        PlannedDefinition definition;
        definition.meshId = static_cast<std::uint32_t>(definitions_.size()) + 1;
        definition.shape = shape;

        Quantity_ColorRGBA shapeColor;
        definition.hasShapeColor = LeafHasColor(label, shapeColor);
        if (definition.hasShapeColor) {
            definition.shapeMaterialId = ResolveMaterial(ToRgba(shapeColor));
        } else {
            CollectFaceColors(label, definition.faceColors);
        }
        if (error_ != model_core::ImportErrorCode::None) {
            unsupportedDefinitions_.insert(key);
            *out = nullptr;
            return false;
        }

        definitions_.push_back(std::move(definition));
        definitionsByKey_.emplace(key, &definitions_.back());
        *out = &definitions_.back();
        return true;
    }

    std::uint32_t AddNode(std::uint32_t parentNodeId, const double transform[16])
    {
        if (nodes_.size() >= limits_.maxNodes) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return 0;
        }
        NodeRecord node;
        node.nodeId = kNodeIdBase + static_cast<std::uint32_t>(nodes_.size());
        node.parentId = parentNodeId;
        std::memcpy(node.transform, transform, sizeof(node.transform));
        nodes_.push_back(node);
        return node.nodeId;
    }

    bool InstanceColor(const TDF_Label& occurrence, Quantity_ColorRGBA& color) const
    {
        if (colorTool_.IsNull()) return false;
        const TopoDS_Shape occurrenceShape = XCAFDoc_ShapeTool::GetShape(occurrence);
        if (occurrenceShape.IsNull()) return false;
        return colorTool_->GetInstanceColor(occurrenceShape, XCAFDoc_ColorGen, color)
            || colorTool_->GetInstanceColor(occurrenceShape, XCAFDoc_ColorSurf, color);
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
            PlannedDefinition* planned = nullptr;
            if (!PlanDefinition(definition, &planned)) {
                path_.erase(pathKey);
                return;
            }
            if (!planned) {
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
            PlannedOccurrence plannedOccurrence;
            plannedOccurrence.nodeId = nodeId;
            std::memcpy(plannedOccurrence.world, world, sizeof(plannedOccurrence.world));
            plannedOccurrence.hasOverride = hasInstanceColor;
            plannedOccurrence.overrideMaterialId = overrideMaterial;
            planned->occurrences.push_back(plannedOccurrence);
            hasOccurrence_ = true;
            path_.erase(pathKey);
            return;
        }

        path_.erase(pathKey); // neither assembly nor shape: not drawn
    }

    Handle(TDocStd_Document) document_;
    Handle(XCAFDoc_ShapeTool) shapeTool_;
    Handle(XCAFDoc_ColorTool) colorTool_;
    StepXdeLimits limits_;
    std::vector<NodeRecord> nodes_;
    std::vector<MaterialRecord> materials_;
    std::deque<PlannedDefinition> definitions_;
    std::unordered_map<std::string, PlannedDefinition*> definitionsByKey_;
    std::unordered_set<std::string> unsupportedDefinitions_;
    std::unordered_map<MaterialKey, std::uint32_t, MaterialKeyHash> materialByKey_;
    std::unordered_set<std::string> path_;
    std::uint32_t warningCount_ = 0;
    bool hasOccurrence_ = false;
    model_core::ImportErrorCode error_ = model_core::ImportErrorCode::None;
    bool cancelled_ = false;
    std::function<bool()> cancelledProbe_;
};

// --- phase B: bounded tessellation and progressive emission -------------------

struct GeometryRecord {
    std::uint32_t chunkId = 0;
    std::uint32_t meshId = 0;
    std::uint32_t materialId = 0;
    std::vector<Vertex> vertices;
};

// Owns exactly one output window. Chunks accumulate until the next one would
// not fit; the window is then handed to the batch publisher and reused. The
// terminal window is finalized in place. Descriptor/payload layout and checksum
// semantics are byte-compatible with the STEP-003 single-window writer.
class SceneEmitter {
public:
    SceneEmitter(std::span<std::byte> section, std::uint64_t generationId, double metersPerUnit,
                 std::uint32_t meshes, std::uint32_t nodes, std::uint32_t warningCount,
                 StepBatchPublisher publish, std::function<bool()> cancelled)
        : section_(section)
        , generationId_(generationId)
        , metersPerUnit_(metersPerUnit)
        , meshCount_(meshes)
        , nodeCount_(nodes)
        , warningCount_(warningCount)
        , publish_(std::move(publish))
        , cancelled_(std::move(cancelled))
    {
    }

    model_core::ImportErrorCode error() const { return error_; }
    std::uint32_t batchesPublished() const { return batchesPublished_; }
    std::uint32_t nextInstanceSerial() const { return nextInstanceSerial_; }

    bool AddNode(const NodeRecord& node)
    {
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
        return Add(descriptor, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&payload), sizeof(payload)));
    }

    bool AddMaterial(const MaterialRecord& material)
    {
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Material;
        descriptor.chunkId = material.chunkId;
        return Add(descriptor, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&material.payload), sizeof(material.payload)));
    }

    // Takes ownership of `chunk.vertices`. Computes the cluster-local float
    // positions and the double origin, writes the geometry chunk, and returns
    // the identity/bounds an occurrence needs to reference it.
    std::optional<EmittedGeometryRef> AddGeometry(GeometryRecord& chunk)
    {
        if (chunk.vertices.empty()) return std::nullopt;
        double origin[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                            std::numeric_limits<double>::max()};
        for (const Vertex& vertex : chunk.vertices) {
            if (!std::isfinite(vertex.px) || !std::isfinite(vertex.py) || !std::isfinite(vertex.pz)) {
                error_ = model_core::ImportErrorCode::MalformedData;
                return std::nullopt;
            }
            origin[0] = (std::min)(origin[0], static_cast<double>(vertex.px));
            origin[1] = (std::min)(origin[1], static_cast<double>(vertex.py));
            origin[2] = (std::min)(origin[2], static_cast<double>(vertex.pz));
        }
        float localMin[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
        float localMax[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                             -std::numeric_limits<float>::max()};
        for (Vertex& vertex : chunk.vertices) {
            vertex.px = static_cast<float>(static_cast<double>(vertex.px) - origin[0]);
            vertex.py = static_cast<float>(static_cast<double>(vertex.py) - origin[1]);
            vertex.pz = static_cast<float>(static_cast<double>(vertex.pz) - origin[2]);
            const float position[3] = {vertex.px, vertex.py, vertex.pz};
            for (int axis = 0; axis < 3; ++axis) {
                localMin[axis] = (std::min)(localMin[axis], position[axis]);
                localMax[axis] = (std::max)(localMax[axis], position[axis]);
            }
        }

        const std::uint64_t vertexBytes = static_cast<std::uint64_t>(chunk.vertices.size()) * sizeof(Vertex);
        const std::uint64_t indexBytes = static_cast<std::uint64_t>(chunk.vertices.size()) * sizeof(std::uint32_t);
        std::vector<std::byte> payload(static_cast<std::size_t>(vertexBytes + indexBytes));
        std::memcpy(payload.data(), chunk.vertices.data(), static_cast<std::size_t>(vertexBytes));
        for (std::uint32_t i = 0; i < chunk.vertices.size(); ++i) {
            std::memcpy(payload.data() + vertexBytes + static_cast<std::size_t>(i) * sizeof(i), &i, sizeof(i));
        }

        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::TriangleList;
        descriptor.chunkId = chunk.chunkId;
        descriptor.vertexLayoutId = static_cast<std::uint32_t>(VertexLayoutId::PositionNormalUv0_F32);
        descriptor.lodLevel = kFineLod;
        descriptor.meshId = chunk.meshId;
        descriptor.vertexCount = static_cast<std::uint32_t>(chunk.vertices.size());
        descriptor.indexCount = static_cast<std::uint32_t>(chunk.vertices.size());
        descriptor.sourceRangeOffset = 0;
        descriptor.sourceRangeLength = descriptor.indexCount;
        descriptor.boundsState = BoundsState::Verified;
        for (int axis = 0; axis < 3; ++axis) {
            descriptor.origin[axis] = origin[axis];
            descriptor.localMin[axis] = localMin[axis];
            descriptor.localMax[axis] = localMax[axis];
        }
        if (!Add(descriptor, payload)) return std::nullopt;

        EmittedGeometryRef reference;
        reference.chunkId = chunk.chunkId;
        reference.materialId = chunk.materialId;
        for (int axis = 0; axis < 3; ++axis) {
            reference.origin[axis] = origin[axis];
            reference.localMin[axis] = localMin[axis];
            reference.localMax[axis] = localMax[axis];
        }
        return reference;
    }

    bool EmitInstance(const EmittedGeometryRef& geometry, std::uint32_t materialId,
                      std::uint32_t nodeId, const double world[16])
    {
        MeshInstancePayload payload{};
        payload.instanceId = kInstanceIdBase + nextInstanceSerial_++;
        payload.nodeId = nodeId;
        payload.geometryChunkId = geometry.chunkId;
        payload.materialChunkId = materialId;
        payload.flags = kSceneRecordVisible;
        if (!TransformBounds(geometry.origin, geometry.localMin, geometry.localMax, world,
                             payload.worldMin, payload.worldMax)) {
            error_ = model_core::ImportErrorCode::MalformedData;
            return false;
        }
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::MeshInstance;
        descriptor.chunkId = payload.instanceId;
        descriptor.dependencyIds[0] = payload.geometryChunkId;
        descriptor.dependencyIds[1] = payload.materialChunkId;
        descriptor.dependencyIds[2] = payload.nodeId;
        descriptor.dependencyCount = payload.materialChunkId ? 3u : 2u;
        return Add(descriptor, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&payload), sizeof(payload)));
    }

    bool AddStatus()
    {
        if (warningCount_ == 0) return true;
        ImportStatusPayload status{};
        status.optionalFeatureWarnings = (std::min)(warningCount_, 64u);
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = kStatusChunkId;
        return Add(descriptor, std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&status), sizeof(status)));
    }

    // Finalizes the terminal window into the section.
    bool Finalize(std::uint32_t& chunkCount, std::uint64_t& sectionBytesWritten)
    {
        if (!FlushToSection()) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return false;
        }
        chunkCount = lastChunkCount_;
        sectionBytesWritten = lastSectionBytes_;
        return true;
    }

private:
    struct PendingChunk {
        ChunkDescriptor descriptor;
        std::vector<std::byte> payload;
    };

    bool Add(const ChunkDescriptor& base, std::span<const std::byte> payload)
    {
        if (cancelled_ && cancelled_()) {
            error_ = model_core::ImportErrorCode::Cancelled;
            return false;
        }
        const std::uint64_t needed = kSectionHeaderSize
            + static_cast<std::uint64_t>(pending_.size() + 1) * kChunkDescriptorSize
            + pendingBytes_ + payload.size();
        if (needed > section_.size()) {
            if (pending_.empty() || !publish_) {
                error_ = model_core::ImportErrorCode::ResourceLimit;
                return false;
            }
            if (!FlushAndPublish()) return false;
            const std::uint64_t recheck = kSectionHeaderSize + kChunkDescriptorSize + payload.size();
            if (recheck > section_.size()) {
                error_ = model_core::ImportErrorCode::ResourceLimit;
                return false;
            }
        }
        PendingChunk pending;
        pending.descriptor = base;
        pending.payload.assign(payload.begin(), payload.end());
        pendingBytes_ += pending.payload.size();
        pending_.push_back(std::move(pending));
        return true;
    }

    // Writes the pending chunks into the caller's section, computes the header
    // and checksums, and leaves the window self-consistent.
    bool FlushToSection()
    {
        if (pending_.empty()) {
            lastChunkCount_ = 0;
            lastSectionBytes_ = kSectionHeaderSize;
            return true;
        }
        std::uint64_t offset = kSectionHeaderSize
            + static_cast<std::uint64_t>(pending_.size()) * kChunkDescriptorSize;
        for (PendingChunk& pending : pending_) {
            pending.descriptor.normalizedRangeOffset = offset;
            pending.descriptor.normalizedRangeLength = pending.payload.size();
            pending.descriptor.byteSize = pending.payload.size();
            pending.descriptor.chunkChecksum = WireChecksum64(
                std::span<const std::byte>(pending.payload.data(), pending.payload.size()));
            offset += pending.payload.size();
            if (offset > section_.size()) return false;
        }
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            std::memcpy(section_.data() + kSectionHeaderSize + i * kChunkDescriptorSize,
                        &pending_[i].descriptor, sizeof(ChunkDescriptor));
            std::memcpy(section_.data() + pending_[i].descriptor.normalizedRangeOffset,
                        pending_[i].payload.data(), pending_[i].payload.size());
        }
        SectionHeader header{};
        header.magic = kSectionMagic;
        header.protocolVersion = kCurrentProtocolVersion;
        header.generationId = generationId_;
        header.sectionLength = offset;
        header.chunkCount = static_cast<std::uint32_t>(pending_.size());
        header.scene.generationId = generationId_;
        header.scene.format = SourceFormatId::Step;
        header.scene.upAxis = UpAxisId::Unknown;
        header.scene.metersPerUnit = metersPerUnit_;
        header.scene.meshCount = meshCount_;
        header.scene.nodeCount = nodeCount_;
        header.scene.animationCount = 0;
        header.scene.skinCount = 0;
        header.scene.boneCount = 0;
        header.sectionChecksum = WireChecksum64(
            section_.subspan(kSectionHeaderSize, static_cast<std::size_t>(offset - kSectionHeaderSize)));
        std::memcpy(section_.data(), &header, sizeof(header));
        lastChunkCount_ = header.chunkCount;
        lastSectionBytes_ = offset;
        return true;
    }

    bool FlushAndPublish()
    {
        if (!FlushToSection()) {
            error_ = model_core::ImportErrorCode::ResourceLimit;
            return false;
        }
        const std::uint32_t chunkCount = lastChunkCount_;
        const std::uint64_t bytes = lastSectionBytes_;
        pending_.clear();
        pendingBytes_ = 0;
        if (!publish_(chunkCount, bytes)) {
            error_ = model_core::ImportErrorCode::Cancelled;
            return false;
        }
        ++batchesPublished_;
        return true;
    }

    std::span<std::byte> section_;
    std::uint64_t generationId_;
    double metersPerUnit_;
    std::uint32_t meshCount_;
    std::uint32_t nodeCount_;
    std::uint32_t warningCount_;
    StepBatchPublisher publish_;
    std::function<bool()> cancelled_;
    std::vector<PendingChunk> pending_;
    std::uint64_t pendingBytes_ = 0;
    std::uint32_t lastChunkCount_ = 0;
    std::uint64_t lastSectionBytes_ = kSectionHeaderSize;
    std::uint32_t batchesPublished_ = 0;
    std::uint32_t nextInstanceSerial_ = 0;
    model_core::ImportErrorCode error_ = model_core::ImportErrorCode::None;
};

class DefinitionMesher {
public:
    DefinitionMesher(const StepTessellationProfile& profile, const StepXdeLimits& limits)
        : profile_(profile), limits_(limits)
    {
    }

    void SetCancellationProbe(std::function<bool()> probe) { cancelledProbe_ = std::move(probe); }

    // STEP-004 work item 7: bounded per-definition mesh-cost accumulator for
    // STEP-005's two-pass-versus-single-pass decision.
    double meshMilliseconds() const { return meshMilliseconds_; }

    // Tessellates `shape` at the display profile and appends bounded,
    // de-indexed, material-homogeneous geometry chunks (each <= the profile's
    // chunkTriangles) to `chunks`.
    model_core::ImportErrorCode Mesh(
        const TopoDS_Shape& shape,
        const std::vector<std::pair<TopoDS_Shape, std::uint32_t>>& faceColors,
        bool hasShapeColor, std::uint32_t shapeMaterialId, std::uint32_t meshId,
        std::uint32_t& nextGeometryId, std::vector<GeometryRecord>& chunks)
    {
        double diagonal = 0.0;
        {
            Bnd_Box box;
            BRepBndLib::Add(shape, box);
            if (!box.IsVoid()) {
                const gp_Pnt low = box.CornerMin();
                const gp_Pnt high = box.CornerMax();
                const double dx = high.X() - low.X();
                const double dy = high.Y() - low.Y();
                const double dz = high.Z() - low.Z();
                diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }

        IMeshTools_Parameters parameters;
        parameters.Deflection = StepDeriveLinearDeflection(
            diagonal, profile_.displayRelativeDeflection, profile_.minAbsoluteDeflection,
            profile_.maxAbsoluteDeflection);
        parameters.Angle = profile_.displayAngularDeflection;
        parameters.MinSize = StepDeriveMinEdge(diagonal, profile_.relativeMinEdge,
                                               profile_.minAbsoluteEdgeLength);
        parameters.Relative = Standard_False;
        parameters.InParallel = profile_.parallel ? Standard_True : Standard_False;
        // Prefer an authored triangulation (e.g. an AP242 tessellated
        // representation) over regenerating one; the pinned reader attaches it
        // to the transferred faces, and BRepMesh keeps it when lowering quality
        // is forbidden.
        parameters.AllowQualityDecrease = Standard_False;

        const auto start = std::chrono::steady_clock::now();
        BRepMesh_IncrementalMesh mesher(shape, parameters);
        mesher.Perform();
        meshMilliseconds_ += ElapsedMilliseconds(start);
        if (!StepWithinDefinitionTime(ElapsedMilliseconds(start), profile_.maxDefinitionMilliseconds))
            return model_core::ImportErrorCode::TessellationFailed;

        std::uint32_t faces = 0, edges = 0;
        for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
            if (++faces > limits_.maxSubshapes) return model_core::ImportErrorCode::TessellationFailed;
        }
        for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
            if (++edges > limits_.maxSubshapes) return model_core::ImportErrorCode::TessellationFailed;
        }
        if (faces > profile_.maxFacesPerDefinition || edges > profile_.maxEdgesPerDefinition)
            return model_core::ImportErrorCode::TessellationFailed;

        // Materials group the definition's triangles. std::map keeps the group
        // ordering deterministic (neutral/0 first) independent of face order.
        std::map<std::uint32_t, std::vector<Vertex>> groups;
        std::uint64_t definitionTriangles = 0;
        std::uint32_t checkedFaces = 0;
        for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
            if ((++checkedFaces % kDefinitionCancelCheckInterval) == 0 && Cancelled())
                return model_core::ImportErrorCode::Cancelled;
            const TopoDS_Face face = TopoDS::Face(explorer.Current());
            const std::uint32_t materialId = hasShapeColor
                ? shapeMaterialId
                : FaceMaterial(face, faceColors);

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

                AppendVertex(groups[materialId], p1, nx, ny, nz);
                AppendVertex(groups[materialId], p2, nx, ny, nz);
                AppendVertex(groups[materialId], p3, nx, ny, nz);
                if (++definitionTriangles > profile_.maxTrianglesPerDefinition)
                    return model_core::ImportErrorCode::TessellationFailed;
            }
        }
        if (definitionTriangles == 0) return model_core::ImportErrorCode::None; // nothing visible

        for (auto& [materialId, vertices] : groups) {
            if (Cancelled()) return model_core::ImportErrorCode::Cancelled;
            std::size_t begin = 0;
            while (begin < vertices.size()) {
                const std::size_t remainingTriangles = (vertices.size() - begin) / 3;
                const std::size_t takeTriangles = (std::min)(
                    remainingTriangles, static_cast<std::size_t>(profile_.chunkTriangles));
                const std::size_t takeVertices = takeTriangles * 3;
                GeometryRecord chunk;
                chunk.chunkId = nextGeometryId++;
                chunk.meshId = meshId;
                chunk.materialId = materialId;
                chunk.vertices.assign(vertices.begin() + static_cast<std::ptrdiff_t>(begin),
                                      vertices.begin() + static_cast<std::ptrdiff_t>(begin + takeVertices));
                chunks.push_back(std::move(chunk));
                begin += takeVertices;
            }
        }
        return model_core::ImportErrorCode::None;
    }

private:
    bool Cancelled() const { return cancelledProbe_ && cancelledProbe_(); }

    static double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    static std::uint32_t FaceMaterial(const TopoDS_Face& face,
                                      const std::vector<std::pair<TopoDS_Shape, std::uint32_t>>& faceColors)
    {
        for (const auto& [subShape, materialId] : faceColors) {
            if (subShape.IsSame(face)) return materialId;
        }
        return 0;
    }

    static void AppendVertex(std::vector<Vertex>& vertices, const gp_Pnt& point, double nx, double ny, double nz)
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

    const StepTessellationProfile& profile_;
    const StepXdeLimits& limits_;
    std::function<bool()> cancelledProbe_;
    double meshMilliseconds_ = 0.0;
};

} // namespace

StepXdeResult RunStepXdeAdapter(const model_core::ParseStepFileRequest& request,
                                std::span<std::byte> section, HANDLE sourceHandle,
                                HANDLE cancellationEvent, const StepXdeLimits& limits,
                                const StepBatchPublisher& publish)
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
    auto closeDocument = [&] {
        if (!document.IsNull()) XCAFApp_Application::GetApplication()->Close(document);
    };

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
            closeDocument();
            return result;
        }
        if (IsCancelled(cancellationEvent)) {
            result.errorCode = model_core::ImportErrorCode::Cancelled;
            closeDocument();
            return result;
        }
        if (!reader.Transfer(document)) {
            result.errorCode = model_core::ImportErrorCode::MalformedData;
            closeDocument();
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
            closeDocument();
            return result;
        }
        // More than one distinct authored length unit is contradictory under
        // the static-preview contract; do not guess which representation wins.
        for (Standard_Integer index = 2; index <= lengthUnits.Length(); ++index) {
            if (std::strcmp(lengthUnits.Value(index).ToCString(),
                            lengthUnits.Value(1).ToCString()) != 0) {
                result.errorCode = model_core::ImportErrorCode::UnsupportedRequiredFeature;
                closeDocument();
                return result;
            }
        }
        double metersPerUnit = 0.0;
        if (!XCAFDoc_DocumentTool::GetLengthUnit(document, metersPerUnit)
            || !std::isfinite(metersPerUnit) || metersPerUnit <= 0.0 || metersPerUnit > 1e12) {
            result.errorCode = model_core::ImportErrorCode::UnsupportedRequiredFeature;
            closeDocument();
            return result;
        }

        // Phase A: bounded planning, no tessellation and no normalized bytes.
        ScenePlanner planner(document, limits);
        planner.SetCancellationProbe([cancellationEvent] { return IsCancelled(cancellationEvent); });
        if (!planner.shapeToolValid()) {
            result.errorCode = model_core::ImportErrorCode::EmptyGeometry;
            closeDocument();
            return result;
        }
        const model_core::ImportErrorCode planned = planner.Build();
        if (planned != model_core::ImportErrorCode::None) {
            result.errorCode = planned;
            closeDocument();
            return result;
        }

        // Phase B: one bounded window at a time. meshCount/nodeCount are known
        // after planning so every progressive batch carries identical
        // generation-wide metadata.
        SceneEmitter emitter(section, request.generationId, metersPerUnit, planner.definitionCount(),
                             static_cast<std::uint32_t>(planner.nodes().size()),
                             planner.warningCount(), publish,
                             [cancellationEvent] { return IsCancelled(cancellationEvent); });
        DefinitionMesher mesher(limits.profile, limits);
        mesher.SetCancellationProbe([cancellationEvent] { return IsCancelled(cancellationEvent); });

        for (const NodeRecord& node : planner.nodes()) {
            if (!emitter.AddNode(node)) break;
        }
        for (const MaterialRecord& material : planner.materials()) {
            if (!emitter.AddMaterial(material)) break;
        }

        std::uint32_t nextGeometryId = kGeometryIdBase;
        for (const PlannedDefinition& definition : planner.definitions()) {
            if (emitter.error() != model_core::ImportErrorCode::None) break;
            std::vector<GeometryRecord> chunks;
            const model_core::ImportErrorCode meshed = mesher.Mesh(
                definition.shape, definition.faceColors, definition.hasShapeColor,
                definition.shapeMaterialId, definition.meshId, nextGeometryId, chunks);
            if (meshed != model_core::ImportErrorCode::None) {
                result.errorCode = meshed;
                closeDocument();
                return result;
            }
            if (chunks.empty()) continue; // unsupported/unmeshed definition

            std::vector<EmittedGeometryRef> emitted;
            emitted.reserve(chunks.size());
            for (GeometryRecord& chunk : chunks) {
                const auto reference = emitter.AddGeometry(chunk);
                if (!reference) break;
                emitted.push_back(*reference);
            }
            if (emitter.error() != model_core::ImportErrorCode::None) break;

            for (const PlannedOccurrence& occurrence : definition.occurrences) {
                for (const EmittedGeometryRef& reference : emitted) {
                    const std::uint32_t materialId = occurrence.hasOverride
                        ? occurrence.overrideMaterialId : reference.materialId;
                    if (!emitter.EmitInstance(reference, materialId, occurrence.nodeId, occurrence.world))
                        break;
                }
                if (emitter.error() != model_core::ImportErrorCode::None) break;
            }
            if (emitter.error() != model_core::ImportErrorCode::None) break;
        }

        if (emitter.error() != model_core::ImportErrorCode::None) {
            result.errorCode = emitter.error();
            closeDocument();
            return result;
        }
        if (!emitter.AddStatus()) {
            result.errorCode = emitter.error();
            closeDocument();
            return result;
        }
        if (IsCancelled(cancellationEvent)) {
            result.errorCode = model_core::ImportErrorCode::Cancelled;
            closeDocument();
            return result;
        }

        std::uint32_t chunkCount = 0;
        std::uint64_t sectionBytesWritten = 0;
        if (!emitter.Finalize(chunkCount, sectionBytesWritten)) {
            result.errorCode = model_core::ImportErrorCode::ResourceLimit;
            closeDocument();
            return result;
        }

        result.chunkCount = chunkCount;
        result.sectionBytesWritten = sectionBytesWritten;
        result.batchCount = emitter.batchesPublished();
        result.definitionCount = planner.definitionCount();
        result.nodeCount = static_cast<std::uint32_t>(planner.nodes().size());
        result.instanceCount = emitter.nextInstanceSerial();
        result.materialCount = static_cast<std::uint32_t>(planner.materials().size());
        result.warningCount = planner.warningCount();
        result.meshMilliseconds = static_cast<std::uint64_t>(mesher.meshMilliseconds());
        closeDocument();
        return result;
    } catch (const std::exception&) {
        closeDocument();
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    } catch (...) {
        closeDocument();
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }
}

} // namespace step_host