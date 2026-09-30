#define NOMINMAX

// T34 STEP/STP family adapter implementation (see StepFamilyAdapter.h).
//
// This is the constrained OCCT adapter ADR-0002 requires: it is compiled and
// linked only into Preview3DThumbnailProvider.dll, Tests.Unit.exe and
// Tests.ProviderHost.exe (the dedicated static OCCT closure is isolated from the
// repository-root manifest so the viewer, the general worker and either import
// host can never link or deploy OCCT). It never launches Preview3DStepHost.exe.
//
// The product-owned Part-21 admission scanner is the host's own source
// (source-not-state, ADR-0004): compatibility-host-step/src/StepPart21Preflight.

#include "StepFamilyAdapter.h"

#include "AllocationLedger.h"
#include "Containment.h"
#include "Deadline.h"
#include "ProviderLimits.h"
#include "ProviderTypes.h"

#include "StepPart21Preflight.h"

#pragma warning(push, 0)
#include <BRepBndLib.hxx>
#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IMeshTools_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_SequenceOfAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDocStd_Document.hxx>
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
#include <XCAFDoc_LayerTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_XYZ.hxx>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace preview3d::provider {
namespace {

// --- Fixed low-detail deterministic policy (T34) ------------------------------
//
// The provider uses the stricter provider ceilings, not the host's Tier-B
// values: a reduced triangle cap, a bounded scratch reservation, and a fixed
// low-detail tessellation profile. Every bound is deterministic and independent
// of the file, so the same input always produces the same samples.
constexpr std::uint32_t kStepMaxEntities = 2'000'000;
constexpr std::uint32_t kStepMaxReferences = 20'000'000;
constexpr std::uint32_t kStepMaxSections = 4096;
constexpr std::uint32_t kStepMaxRecordBytes = 1u << 20;
constexpr std::uint32_t kStepMaxStringBytes = 1u << 20;

constexpr std::uint32_t kStepMaxDefinitions = 20'000;
constexpr std::uint32_t kStepMaxHierarchyDepth = 256;
constexpr std::uint32_t kStepMaxTrianglesPerDefinition = 1'000'000;
constexpr std::uint64_t kStepMaxTrianglesTotal = 2'000'000;
constexpr std::uint64_t kStepMaxGeometryCacheTriangles = 750'000;

constexpr std::uint64_t kStepScratchReservationBytes = 96ull * 1024 * 1024;
constexpr std::size_t kStepReadBlockBytes = 64 * 1024;

// Low-detail deterministic meshing. Relative deflection is a fraction of the
// definition bounding diagonal, clamped into an absolute window so a tiny
// feature and a large assembly are both bounded.
constexpr double kStepRelativeDeflection = 0.05;
constexpr double kStepMinAbsoluteDeflection = 0.01;
constexpr double kStepMaxAbsoluteDeflection = 5.0;
constexpr double kStepAngularDeflection = 0.7;

constexpr std::uint32_t kInvalidDefinition = 0xFFFF'FFFFu;

using SteadyClock = std::chrono::steady_clock;

double ElapsedMilliseconds(const SteadyClock::time_point& start) noexcept
{
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}

// --- affine helpers (row-vector p * M) ---------------------------------------

void IdentityAffine(double out[16]) noexcept
{
    for (int i = 0; i < 16; ++i) out[i] = 0.0;
    out[0] = out[5] = out[10] = out[15] = 1.0;
}

bool FiniteAffine(const double m[16]) noexcept
{
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::abs(m[i]) > 1e30) return false;
    return true;
}

// gp_Trsf maps p -> L*p + t. The wire contract here is row-vector (p * M), so
// the upper-left 3x3 is the transpose of the gp_Trsf linear part and the
// translation occupies elements 12..14.
bool TrsfToAffine(const gp_Trsf& transform, double out[16]) noexcept
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

bool MultiplyAffine(const double left[16], const double right[16], double out[16]) noexcept
{
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            double value = 0.0;
            for (std::uint32_t k = 0; k < 4; ++k) value += left[row * 4 + k] * right[k * 4 + column];
            if (!std::isfinite(value) || std::abs(value) > 1e30) return false;
            out[row * 4 + column] = value;
        }
    }
    return FiniteAffine(out);
}

void TransformPoint(const double point[3], const double m[16], double out[3]) noexcept
{
    for (std::uint32_t axis = 0; axis < 3; ++axis)
        out[axis] = point[0] * m[axis] + point[1] * m[4 + axis] + point[2] * m[8 + axis] + m[12 + axis];
}

// Transforms a local direction (position offset or normal) by the linear part.
void TransformDirection(const float vector[3], const double m[16], float out[3]) noexcept
{
    for (std::uint32_t axis = 0; axis < 3; ++axis)
        out[axis] = static_cast<float>(vector[0] * m[axis] + vector[1] * m[4 + axis] + vector[2] * m[8 + axis]);
}

float SrgbToLinear(float value) noexcept
{
    return Quantity_Color::Convert_sRGB_To_LinearRGB((std::max)(0.0f, (std::min)(1.0f, value)));
}

struct Rgba {
    float r = 0.8f, g = 0.8f, b = 0.8f, a = 1.0f;
};

Rgba ToRgba(const Quantity_ColorRGBA& color) noexcept
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

std::uint32_t FloatBits(float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

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

// --- bounded seekable stream over the Shell BoundedSource --------------------
//
// OCCT's reader needs a seekable std::istream. The whole file is never copied:
// reads go through BoundedSource::ReadAt under the provider deadline and stream
// ceiling. When the T12 source already materialized its 128 MiB contiguous
// backing, SpanStreamBuf serves the same bytes with no I/O at all.
class SourceStreamBuf final : public std::streambuf {
public:
    SourceStreamBuf(BoundedSource* source, Deadline* deadline, std::uint64_t size)
        : source_(source), deadline_(deadline), size_(size)
    {
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }

    bool failed() const noexcept { return failed_; }

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
    std::uint64_t CurrentOffset() const noexcept
    {
        return bufferStart_ + static_cast<std::uint64_t>(gptr() - buffer_.data());
    }

    bool Fill(std::uint64_t offset)
    {
        if (failed_ || offset >= size_) return false;
        if (deadline_ != nullptr && !deadline_->Checkpoint()) {
            failed_ = true;
            return false;
        }
        const auto want = static_cast<std::size_t>(
            (std::min)(static_cast<std::uint64_t>(buffer_.size()), size_ - offset));
        if (!source_->ReadAt(offset, std::span<std::byte>(
                                       reinterpret_cast<std::byte*>(buffer_.data()), want))) {
            failed_ = true;
            return false;
        }
        bufferStart_ = offset;
        setg(buffer_.data(), buffer_.data(), buffer_.data() + want);
        return true;
    }

    BoundedSource* source_ = nullptr;
    Deadline* deadline_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t bufferStart_ = 0;
    bool failed_ = false;
    std::array<char, kStepReadBlockBytes> buffer_{};
};

class SpanStreamBuf final : public std::streambuf {
public:
    explicit SpanStreamBuf(std::span<const std::byte> bytes)
        : base_(reinterpret_cast<char*>(const_cast<std::byte*>(bytes.data()))), size_(bytes.size())
    {
        setg(base_, base_, base_ + size_);
    }

protected:
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override
    {
        const auto current = static_cast<std::int64_t>(gptr() - base_);
        std::int64_t target = 0;
        if (direction == std::ios_base::cur) target = current + offset;
        else if (direction == std::ios_base::end) target = static_cast<std::int64_t>(size_) + offset;
        else target = offset;
        target = (std::max)(std::int64_t{0}, (std::min)(target, static_cast<std::int64_t>(size_)));
        setg(base_, base_ + target, base_ + size_);
        return pos_type(target);
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
    {
        return seekoff(off_type(position), std::ios_base::beg, mode);
    }

private:
    char* base_ = nullptr;
    std::uint64_t size_ = 0;
};

using ErrorCode = model_core::ImportErrorCode;

} // namespace

// --- opaque implementation ---------------------------------------------------

struct StepAdapter::Impl {
    AdapterInput input{};
    bool parsed = false;

    BoundedSource* source = nullptr;
    Deadline* deadline = nullptr;
    std::uint64_t sourceSize = 0;
    std::span<const std::byte> contiguous{};
    std::optional<AllocationReservation> scratchReservation;

    Handle(TDocStd_Document) document;

    struct PlanDefinition {
        TopoDS_Shape shape;
        std::uint32_t baseMaterial = 1;
        std::string entry;
    };
    struct Occurrence {
        std::uint32_t definition = kInvalidDefinition;
        double world[16]{};
        bool hasOverride = false;
        std::uint32_t overrideMaterial = 1;
    };

    std::vector<model_core::MaterialPayload> materials_;
    std::unordered_map<MaterialKey, std::uint32_t, MaterialKeyHash> materialByKey_;
    std::vector<PlanDefinition> definitions_;
    std::vector<Occurrence> occurrences_;
    std::unordered_map<std::string, std::uint32_t> definitionByEntry_;
    std::unordered_set<std::string> unsupportedDefinitions_;

    bool externalDocumentRejected = false;
    bool admissionRejected = false;
    std::uint64_t inspectedTriangles = 0;
    std::uint64_t parseMs = 0;

    ~Impl() noexcept { Reset(); }

    void Reset() noexcept
    {
        if (!document.IsNull()) {
            try {
                XCAFApp_Application::GetApplication()->Close(document);
            } catch (...) {
            }
            document.Nullify();
        }
        input = AdapterInput{};
        parsed = false;
        source = nullptr;
        deadline = nullptr;
        sourceSize = 0;
        contiguous = {};
        scratchReservation.reset();
        materials_.clear();
        materialByKey_.clear();
        definitions_.clear();
        occurrences_.clear();
        definitionByEntry_.clear();
        unsupportedDefinitions_.clear();
        externalDocumentRejected = false;
        admissionRejected = false;
        inspectedTriangles = 0;
        parseMs = 0;
    }

    ErrorCode SourceReadFailure() const noexcept
    {
        if (deadline != nullptr && !deadline->Checkpoint()) return ErrorCode::Cancelled;
        return ErrorCode::MalformedData;
    }

    ErrorCode RunAdmission()
    {
        step_host::StepPreflightLimits limits;
        limits.maxLexedBytes = ProviderLimits::kStreamMaxBytes;
        limits.maxEntityRecords = kStepMaxEntities;
        limits.maxReferenceCount = kStepMaxReferences;
        limits.maxNestingDepth = 256;
        limits.maxRecordBytes = kStepMaxRecordBytes;
        limits.maxStringBytes = kStepMaxStringBytes;
        limits.maxDataSections = kStepMaxSections;
        limits.maxExternalDocuments = 0;

        step_host::StepPart21Scanner scanner(limits);
        if (!contiguous.empty()) {
            scanner.Feed(contiguous);
        } else {
            std::array<std::byte, kStepReadBlockBytes> buffer{};
            for (std::uint64_t offset = 0; offset < sourceSize;) {
                if (deadline != nullptr && !deadline->Checkpoint()) return ErrorCode::Cancelled;
                const auto take = static_cast<std::size_t>(
                    (std::min)(static_cast<std::uint64_t>(buffer.size()), sourceSize - offset));
                if (!source->ReadAt(offset,
                                    std::span<std::byte>(buffer.data(), take)))
                    return SourceReadFailure();
                if (!scanner.Feed(std::span<const std::byte>(buffer.data(), take))) break;
                offset += take;
            }
        }
        scanner.Finish();

        using step_host::StepPreflightStatus;
        switch (scanner.Result().status) {
            case StepPreflightStatus::Ok:
                return ErrorCode::None;
            case StepPreflightStatus::UnsupportedEncoding:
                admissionRejected = true;
                return ErrorCode::UnsupportedEncoding;
            case StepPreflightStatus::ExternalDocument:
                externalDocumentRejected = true;
                return ErrorCode::UnsupportedRequiredFeature;
            case StepPreflightStatus::NotPart21:
            case StepPreflightStatus::MalformedSyntax:
            case StepPreflightStatus::DuplicateEntity:
            case StepPreflightStatus::ReadFailure:
                admissionRejected = true;
                return ErrorCode::MalformedData;
            case StepPreflightStatus::EntityLimit:
            case StepPreflightStatus::ReferenceLimit:
            case StepPreflightStatus::DepthLimit:
            case StepPreflightStatus::RecordLengthLimit:
            case StepPreflightStatus::StringLengthLimit:
            case StepPreflightStatus::SourceLimit:
                admissionRejected = true;
                return ErrorCode::ResourceLimit;
        }
        return ErrorCode::MalformedData;
    }

    // OCCT's read/transfer/XDE build is an opaque third-party call on the
    // Shell's calling thread. It runs under the T16 exception/SEH containment
    // boundary so one hostile file cannot take down the surrogate; a contained
    // fault returns a typed failure and never a fabricated success.
    ErrorCode LoadDocument() noexcept
    {
        loadResult_ = ErrorCode::None;
        loadCompleted_ = false;
        const ContainmentResult contained =
            RunContained(&Impl::LoadContained, this, deadline, DiagnosticStage::Parse);
        if (!loadCompleted_) {
            if (contained.outcome == ProviderOutcome::OutOfMemory) return ErrorCode::OutOfMemory;
            if (contained.structuredException) return ErrorCode::MalformedData;
            return ErrorCode::InternalImporterFailure;
        }
        if (contained.outcome == ProviderOutcome::Deadline) return ErrorCode::Cancelled;
        return loadResult_;
    }

    static ProviderOutcome LoadContained(void* self)
    {
        auto* impl = static_cast<Impl*>(self);
        try {
            impl->loadResult_ = impl->LoadDocumentInner();
            impl->loadCompleted_ = true;
        } catch (const Standard_Failure&) {
            // An OCCT-raised CAD/kernel error on the accepted file is bad input.
            impl->loadResult_ = ErrorCode::MalformedData;
            impl->loadCompleted_ = true;
        }
        return ClassifyError(impl->loadResult_);
    }

    ErrorCode LoadDocumentInner()
    {
        XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", document);
        if (document.IsNull()) return ErrorCode::InternalImporterFailure;

        STEPCAFControl_Reader reader;
        reader.SetColorMode(Standard_True);
        reader.SetNameMode(Standard_False);
        reader.SetLayerMode(Standard_True);
        reader.SetPropsMode(Standard_False);
        reader.SetGDTMode(Standard_False);
        reader.SetViewMode(Standard_False);

        std::unique_ptr<std::streambuf> buffer;
        if (!contiguous.empty())
            buffer = std::make_unique<SpanStreamBuf>(contiguous);
        else
            buffer = std::make_unique<SourceStreamBuf>(source, deadline, sourceSize);
        std::istream stream(buffer.get());

        IFSelect_ReturnStatus readStatus = IFSelect_RetFail;
        try {
            readStatus = reader.ReadStream("preview3d.step", stream);
        } catch (const Standard_Failure&) {
            // An adversarial Part-21 topology can make OCCT raise while building
            // its model. That is bad input, not a product defect. A non-OCCT
            // fault still reaches the containment boundary.
            return ErrorCode::MalformedData;
        }
        if (readStatus != IFSelect_RetDone) return ErrorCode::MalformedData;
        if (deadline != nullptr && !deadline->Checkpoint()) return ErrorCode::Cancelled;
        if (reader.ChangeReader().NbRootsForTransfer() <= 0) return ErrorCode::EmptyGeometry;

        bool transferred = false;
        try {
            transferred = reader.Transfer(document);
        } catch (const Standard_Failure&) {
            return ErrorCode::MalformedData;
        }
        if (!transferred) return ErrorCode::MalformedData;

        // A self-contained file must author exactly one length unit and a
        // verified metre factor; absent or contradictory units fail closed.
        TColStd_SequenceOfAsciiString lengthUnits, angleUnits, solidAngleUnits;
        reader.ChangeReader().FileUnits(lengthUnits, angleUnits, solidAngleUnits);
        if (lengthUnits.Length() == 0) return ErrorCode::UnsupportedRequiredFeature;
        for (Standard_Integer index = 2; index <= lengthUnits.Length(); ++index) {
            if (std::strcmp(lengthUnits.Value(index).ToCString(),
                            lengthUnits.Value(1).ToCString()) != 0)
                return ErrorCode::UnsupportedRequiredFeature;
        }
        double metersPerUnit = 0.0;
        if (!XCAFDoc_DocumentTool::GetLengthUnit(document, metersPerUnit)
            || !std::isfinite(metersPerUnit) || metersPerUnit <= 0.0 || metersPerUnit > 1e12)
            return ErrorCode::UnsupportedRequiredFeature;

        return BuildPlan();
    }

    std::uint32_t ResolveMaterial(const Rgba& color)
    {
        const MaterialKey key{FloatBits(SrgbToLinear(color.r)), FloatBits(SrgbToLinear(color.g)),
                              FloatBits(SrgbToLinear(color.b)),
                              FloatBits((std::max)(0.0f, (std::min)(1.0f, color.a)))};
        const auto existing = materialByKey_.find(key);
        if (existing != materialByKey_.end()) return existing->second;
        if (materials_.size() >= ProviderLimits::kMaterialsMax) return 1; // neutral, bounded

        model_core::MaterialPayload payload{};
        payload.baseColorFactor[0] = SrgbToLinear(color.r);
        payload.baseColorFactor[1] = SrgbToLinear(color.g);
        payload.baseColorFactor[2] = SrgbToLinear(color.b);
        const float alpha = (std::max)(0.0f, (std::min)(1.0f, color.a));
        payload.baseColorFactor[3] = alpha;
        payload.metallicFactor = 0.0f;
        payload.roughnessFactor = 0.5f;
        payload.emissiveFactor[0] = payload.emissiveFactor[1] = payload.emissiveFactor[2] = 0.0f;
        payload.uvOffset[0] = payload.uvOffset[1] = 0.0f;
        payload.uvScale[0] = payload.uvScale[1] = 1.0f;
        payload.uvRotation = 0.0f;
        payload.alphaMode = alpha < 0.999f ? static_cast<std::uint32_t>(model_core::AlphaModeId::Blend)
                                           : static_cast<std::uint32_t>(model_core::AlphaModeId::Opaque);
        payload.alphaCutoff = 0.5f;
        payload.flags = 0;
        payload.transmissionFactor = 0.0f;
        materials_.push_back(payload);
        const auto index = static_cast<std::uint32_t>(materials_.size());
        materialByKey_.emplace(key, index);
        return index;
    }

    bool LeafHasColor(const TDF_Label& label, Quantity_ColorRGBA& color) const
    {
        if (colorTool.IsNull()) return false;
        return colorTool->GetColor(label, XCAFDoc_ColorSurf, color)
            || colorTool->GetColor(label, XCAFDoc_ColorGen, color)
            || colorTool->GetColor(label, XCAFDoc_ColorCurv, color);
    }

    bool InstanceColor(const TDF_Label& occurrence, Quantity_ColorRGBA& color) const
    {
        if (colorTool.IsNull()) return false;
        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(occurrence);
        if (shape.IsNull()) return false;
        return colorTool->GetInstanceColor(shape, XCAFDoc_ColorGen, color)
            || colorTool->GetInstanceColor(shape, XCAFDoc_ColorSurf, color);
    }

    bool Visible(const TDF_Label& label) const
    {
        if (!XCAFDoc_ColorTool::IsVisible(label)) return false;
        if (layerTool.IsNull()) return true;
        TDF_LabelSequence layers;
        if (!layerTool->GetLayers(label, layers) || layers.Length() == 0) return true;
        for (Standard_Integer index = 1; index <= layers.Length(); ++index)
            if (layerTool->IsVisible(layers.Value(index))) return true;
        return false;
    }

    std::uint32_t PlanDefinitionFor(const TDF_Label& definition, ErrorCode& error)
    {
        TCollection_AsciiString entry;
        TDF_Tool::Entry(definition, entry);
        const std::string key(entry.ToCString());
        const auto existing = definitionByEntry_.find(key);
        if (existing != definitionByEntry_.end()) return existing->second;
        if (unsupportedDefinitions_.find(key) != unsupportedDefinitions_.end())
            return kInvalidDefinition;

        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(definition);
        if (shape.IsNull()) {
            unsupportedDefinitions_.insert(key);
            return kInvalidDefinition;
        }
        if (definitions_.size() >= kStepMaxDefinitions) {
            error = ErrorCode::ResourceLimit;
            return kInvalidDefinition;
        }

        PlanDefinition planned;
        planned.shape = shape;
        planned.entry = key;
        Quantity_ColorRGBA color;
        planned.baseMaterial = LeafHasColor(definition, color) ? ResolveMaterial(ToRgba(color)) : 1u;

        definitions_.push_back(std::move(planned));
        const auto index = static_cast<std::uint32_t>(definitions_.size() - 1);
        definitionByEntry_.emplace(key, index);
        return index;
    }

    ErrorCode BuildPlan()
    {
        shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
        colorTool = XCAFDoc_DocumentTool::ColorTool(document->Main());
        layerTool = XCAFDoc_DocumentTool::LayerTool(document->Main());
        if (shapeTool.IsNull()) return ErrorCode::EmptyGeometry;

        materials_.push_back(NeutralMaterial()); // index 0 -> material index 1

        TDF_LabelSequence roots;
        shapeTool->GetFreeShapes(roots);
        if (roots.Length() == 0) return ErrorCode::EmptyGeometry;

        ErrorCode error = ErrorCode::None;
        for (Standard_Integer index = 1; index <= roots.Length(); ++index) {
            if (deadline != nullptr && !deadline->Checkpoint()) return ErrorCode::Cancelled;
            double identity[16];
            IdentityAffine(identity);
            Visit(roots.Value(index), roots.Value(index), identity, false, 1, error);
            if (error != ErrorCode::None) return error;
        }
        if (occurrences_.empty()) return ErrorCode::EmptyGeometry;
        return ErrorCode::None;
    }

    void Visit(const TDF_Label& occurrence, const TDF_Label& definition,
               const double parentWorld[16], bool hasLocation, std::uint32_t depth,
               ErrorCode& error)
    {
        if (error != ErrorCode::None) return;
        if (depth > kStepMaxHierarchyDepth) {
            error = ErrorCode::ResourceLimit;
            return;
        }
        if (!Visible(occurrence) || !Visible(definition)) return;

        TCollection_AsciiString entry;
        TDF_Tool::Entry(definition, entry);
        const std::string key(entry.ToCString());
        if (!path_.insert(key).second) {
            error = ErrorCode::MalformedData; // assembly cycle
            return;
        }

        double local[16];
        if (hasLocation) {
            if (!TrsfToAffine(XCAFDoc_ShapeTool::GetLocation(occurrence), local)) {
                path_.erase(key);
                error = ErrorCode::MalformedData;
                return;
            }
        } else {
            IdentityAffine(local);
        }
        double world[16];
        if (!MultiplyAffine(local, parentWorld, world)) {
            path_.erase(key);
            error = ErrorCode::MalformedData;
            return;
        }

        if (XCAFDoc_ShapeTool::IsAssembly(definition)) {
            TDF_LabelSequence components;
            XCAFDoc_ShapeTool::GetComponents(definition, components);
            for (Standard_Integer index = 1; index <= components.Length(); ++index) {
                const TDF_Label component = components.Value(index);
                TDF_Label referred;
                const bool isReference = XCAFDoc_ShapeTool::GetReferredShape(component, referred)
                    && !referred.IsNull();
                Visit(component, isReference ? referred : component, world, true, depth + 1, error);
                if (error != ErrorCode::None) break;
            }
            path_.erase(key);
            return;
        }

        if (XCAFDoc_ShapeTool::IsSimpleShape(definition) || XCAFDoc_ShapeTool::IsReference(definition)) {
            const std::uint32_t planned = PlanDefinitionFor(definition, error);
            if (error != ErrorCode::None) {
                path_.erase(key);
                return;
            }
            if (planned != kInvalidDefinition) {
                Occurrence instance;
                instance.definition = planned;
                std::memcpy(instance.world, world, sizeof(instance.world));
                Quantity_ColorRGBA instanceColor;
                if (hasLocation && InstanceColor(occurrence, instanceColor)) {
                    instance.hasOverride = true;
                    instance.overrideMaterial = ResolveMaterial(ToRgba(instanceColor));
                }
                occurrences_.push_back(instance);
            }
            path_.erase(key);
            return;
        }

        path_.erase(key); // neither assembly nor shape: not drawn
    }

    // --- tessellation + emission ---------------------------------------------

    struct CachedGeometry {
        double origin[3] = {0.0, 0.0, 0.0};
        std::vector<float> positions;  // 9 floats per triangle
        std::vector<float> normals;    // 9 floats per triangle
        std::vector<std::uint32_t> materials; // 1 per triangle
        bool built = false;
    };

    // OCCT meshing and triangulation extraction also run under the containment
    // boundary: malformed authored tessellation can fault inside the kernel.
    ErrorCode BuildGeometry(const TopoDS_Shape& shape, CachedGeometry& out) noexcept
    {
        meshResult_ = ErrorCode::None;
        meshCompleted_ = false;
        activeMeshShape_ = &shape;
        activeMeshOut_ = &out;
        const ContainmentResult contained =
            RunContained(&Impl::MeshContained, this, deadline, DiagnosticStage::Geometry);
        activeMeshShape_ = nullptr;
        activeMeshOut_ = nullptr;
        if (!meshCompleted_) {
            if (contained.outcome == ProviderOutcome::OutOfMemory) return ErrorCode::OutOfMemory;
            if (contained.structuredException) return ErrorCode::MalformedData;
            return ErrorCode::TessellationFailed;
        }
        if (contained.outcome == ProviderOutcome::Deadline) return ErrorCode::Cancelled;
        return meshResult_;
    }

    static ProviderOutcome MeshContained(void* self)
    {
        auto* impl = static_cast<Impl*>(self);
        try {
            impl->meshResult_ = impl->BuildGeometryInner(*impl->activeMeshShape_, *impl->activeMeshOut_);
            impl->meshCompleted_ = true;
        } catch (const Standard_Failure&) {
            impl->meshResult_ = ErrorCode::TessellationFailed;
            impl->meshCompleted_ = true;
        }
        return ClassifyError(impl->meshResult_);
    }

    ErrorCode BuildGeometryInner(const TopoDS_Shape& shape, CachedGeometry& out)
    {
        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        double diagonal = 0.0;
        if (!box.IsVoid()) {
            const gp_Pnt low = box.CornerMin();
            const gp_Pnt high = box.CornerMax();
            out.origin[0] = low.X();
            out.origin[1] = low.Y();
            out.origin[2] = low.Z();
            const double dx = high.X() - low.X();
            const double dy = high.Y() - low.Y();
            const double dz = high.Z() - low.Z();
            diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        IMeshTools_Parameters parameters;
        parameters.Deflection = (std::max)(kStepMinAbsoluteDeflection,
            (std::min)(kStepMaxAbsoluteDeflection, diagonal * kStepRelativeDeflection));
        parameters.Angle = kStepAngularDeflection;
        parameters.MinSize = 0.0;
        parameters.Relative = Standard_False;
        parameters.InParallel = Standard_False; // deterministic, no surrogate-sized pool
        parameters.AllowQualityDecrease = Standard_False;

        try {
            BRepMesh_IncrementalMesh mesher(shape, parameters);
            (void)mesher;
        } catch (const Standard_Failure&) {
            return ErrorCode::TessellationFailed;
        }

        for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
            if (deadline != nullptr && !deadline->Checkpoint()) return ErrorCode::Cancelled;
            const TopoDS_Face face = TopoDS::Face(explorer.Current());
            TopLoc_Location location;
            const Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);
            if (triangulation.IsNull() || triangulation->NbTriangles() == 0) continue;
            if (!triangulation->HasNormals()) {
                try {
                    BRepLib_ToolTriangulatedShape::ComputeNormals(face, triangulation);
                } catch (const Standard_Failure&) {
                }
            }
            const bool smooth = triangulation->HasNormals();
            const bool reversed = face.Orientation() == TopAbs_REVERSED;
            const gp_Trsf& transform = location.Transformation();

            for (Standard_Integer index = 1; index <= triangulation->NbTriangles(); ++index) {
                if (out.materials.size() >= kStepMaxTrianglesPerDefinition)
                    return ErrorCode::TessellationFailed;
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
                if (!(length > 1e-30) || !std::isfinite(length)) continue;
                nx /= length; ny /= length; nz /= length;

                const gp_Pnt points[3] = {p1, p2, p3};
                const Standard_Integer nodes[3] = {n1, n2, n3};
                for (int corner = 0; corner < 3; ++corner) {
                    const gp_Pnt& point = points[corner];
                    out.positions.push_back(static_cast<float>(point.X() - out.origin[0]));
                    out.positions.push_back(static_cast<float>(point.Y() - out.origin[1]));
                    out.positions.push_back(static_cast<float>(point.Z() - out.origin[2]));

                    double vx = nx, vy = ny, vz = nz;
                    if (smooth) {
                        gp_Vec3f localNormal;
                        triangulation->Normal(nodes[corner], localNormal);
                        gp_Vec worldNormal(localNormal.x(), localNormal.y(), localNormal.z());
                        worldNormal.Transform(transform);
                        const double magnitude = worldNormal.Magnitude();
                        if (std::isfinite(magnitude) && magnitude > 1e-30) {
                            vx = worldNormal.X() / magnitude;
                            vy = worldNormal.Y() / magnitude;
                            vz = worldNormal.Z() / magnitude;
                            if (vx * nx + vy * ny + vz * nz < 0) {
                                vx = -vx; vy = -vy; vz = -vz;
                            }
                        }
                    }
                    out.normals.push_back(static_cast<float>(vx));
                    out.normals.push_back(static_cast<float>(vy));
                    out.normals.push_back(static_cast<float>(vz));
                }
                out.materials.push_back(1u); // replaced per occurrence/definition
            }
        }
        out.built = true;
        return ErrorCode::None;
    }

    // OCCT process-global document application is created once (standard OCCT
    // lifetime) but every document it holds is per call and closed on Reset.
    Handle(XCAFDoc_ShapeTool) shapeTool;
    Handle(XCAFDoc_ColorTool) colorTool;
    Handle(XCAFDoc_LayerTool) layerTool;
    std::unordered_set<std::string> path_;

    // Transient state for the T16 containment callbacks.
    ErrorCode loadResult_ = ErrorCode::None;
    bool loadCompleted_ = false;
    const TopoDS_Shape* activeMeshShape_ = nullptr;
    CachedGeometry* activeMeshOut_ = nullptr;
    ErrorCode meshResult_ = ErrorCode::None;
    bool meshCompleted_ = false;
};

// --- public adapter surface --------------------------------------------------

StepAdapter::StepAdapter() noexcept
{
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        impl_ = nullptr;
    }
}

StepAdapter::~StepAdapter() noexcept = default;

ErrorCode StepAdapter::Initialize(const AdapterInput& input) noexcept
{
    if (!impl_) return ErrorCode::InternalImporterFailure;
    impl_->Reset();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr)
        return ErrorCode::InternalImporterFailure;
    impl_->input = input;
    return ErrorCode::None;
}

ErrorCode StepAdapter::Parse() noexcept
{
    if (!impl_) return ErrorCode::InternalImporterFailure;
    if (impl_->input.source == nullptr || impl_->input.deadline == nullptr)
        return ErrorCode::InternalImporterFailure;
    // Cooperative stop point: an already-expired deadline fails before any read
    // or OCCT call. OCCT's opaque calls cannot be interrupted mid-call; the
    // pipeline's after-the-fact containment check rejects a later overrun.
    if (!impl_->input.deadline->Checkpoint()) return ErrorCode::Cancelled;

    const auto start = SteadyClock::now();
    impl_->parsed = false;
    impl_->source = impl_->input.source;
    impl_->deadline = impl_->input.deadline;
    impl_->sourceSize = impl_->source->Size();
    impl_->contiguous = impl_->source->ContiguousView();
    if (!impl_->contiguous.empty()) impl_->sourceSize = impl_->contiguous.size();

    if (impl_->sourceSize == 0) return ErrorCode::MalformedData;
    if (impl_->sourceSize > ProviderLimits::kStreamMaxBytes) return ErrorCode::ResourceLimit;

    // A non-seekable stream must fit the bounded contiguous backing; a
    // seekable over-128 MiB stream is read through bounded range reads.
    if (!impl_->source->Seekable() && impl_->contiguous.empty())
        return ErrorCode::ResourceLimit;

    ErrorCode result = ErrorCode::None;
    try {
        result = impl_->RunAdmission();
        if (result == ErrorCode::None) {
            if (impl_->input.ledger != nullptr) {
                impl_->scratchReservation =
                    impl_->input.ledger->ReserveScoped(kStepScratchReservationBytes);
                if (!impl_->scratchReservation.has_value()) return ErrorCode::ResourceLimit;
            }
            result = impl_->LoadDocument();
        }
    } catch (const Standard_Failure&) {
        result = ErrorCode::MalformedData;
    } catch (const std::exception&) {
        result = ErrorCode::InternalImporterFailure;
    } catch (...) {
        result = ErrorCode::InternalImporterFailure;
    }

    impl_->parseMs = static_cast<std::uint64_t>(ElapsedMilliseconds(start));
    if (result != ErrorCode::None) {
        impl_->parsed = false;
        return result;
    }
    impl_->parsed = true;
    return ErrorCode::None;
}

ErrorCode StepAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    if (!impl_ || !impl_->parsed) return ErrorCode::InternalImporterFailure;
    for (std::size_t index = 0; index < impl_->materials_.size(); ++index) {
        if (!sink.OnMaterial(static_cast<std::uint32_t>(index + 1), impl_->materials_[index]))
            break;
    }
    return ErrorCode::None;
}

ErrorCode StepAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    if (!impl_ || !impl_->parsed) return ErrorCode::InternalImporterFailure;

    try {
        std::unordered_map<std::uint32_t, Impl::CachedGeometry> cache;
        std::uint64_t cachedTriangles = 0;
        for (const Impl::Occurrence& occurrence : impl_->occurrences_) {
            if (impl_->deadline != nullptr && !impl_->deadline->Checkpoint())
                return ErrorCode::Cancelled;
            if (occurrence.definition >= impl_->definitions_.size()) continue;

            Impl::CachedGeometry* geometry = nullptr;
            std::optional<Impl::CachedGeometry> temporary;
            const auto found = cache.find(occurrence.definition);
            if (found != cache.end()) {
                geometry = &found->second;
            } else {
                Impl::CachedGeometry built;
                const ErrorCode build = impl_->BuildGeometry(
                    impl_->definitions_[occurrence.definition].shape, built);
                if (build != ErrorCode::None) return build;
                if (cachedTriangles + built.materials.size() <= kStepMaxGeometryCacheTriangles) {
                    cachedTriangles += built.materials.size();
                    geometry = &cache.emplace(occurrence.definition, std::move(built)).first->second;
                } else {
                    temporary = std::move(built);
                    geometry = &*temporary;
                }
            }

            const std::uint32_t definitionMaterial = impl_->definitions_[occurrence.definition].baseMaterial;
            const std::size_t triangles = geometry->materials.size();
            for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
                if (++impl_->inspectedTriangles > kStepMaxTrianglesTotal)
                    return ErrorCode::None; // cap stop, not a failure
                const std::uint32_t material = occurrence.hasOverride
                    ? occurrence.overrideMaterial
                    : definitionMaterial;
                TriangleSample sample{};
                TransformPoint(geometry->origin, occurrence.world, sample.origin);
                const std::size_t base = triangle * 9;
                for (int corner = 0; corner < 3; ++corner) {
                    VertexSample& vertex = sample.vertices[corner];
                    float local[3] = {geometry->positions[base + corner * 3 + 0],
                                      geometry->positions[base + corner * 3 + 1],
                                      geometry->positions[base + corner * 3 + 2]};
                    TransformDirection(local, occurrence.world, vertex.position);
                    float normal[3] = {geometry->normals[base + corner * 3 + 0],
                                       geometry->normals[base + corner * 3 + 1],
                                       geometry->normals[base + corner * 3 + 2]};
                    TransformDirection(normal, occurrence.world, vertex.normal);
                    const float magnitude = std::sqrt(vertex.normal[0] * vertex.normal[0]
                        + vertex.normal[1] * vertex.normal[1] + vertex.normal[2] * vertex.normal[2]);
                    if (magnitude > 1e-12f) {
                        vertex.normal[0] /= magnitude;
                        vertex.normal[1] /= magnitude;
                        vertex.normal[2] /= magnitude;
                    } else {
                        vertex.normal[0] = vertex.normal[1] = vertex.normal[2] = 0.0f;
                    }
                    vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1.0f;
                }
                sample.materialIndex = material;
                if (!sink.OnTriangle(sample)) return ErrorCode::None;
            }
        }
        return ErrorCode::None;
    } catch (const Standard_Failure&) {
        return ErrorCode::TessellationFailed;
    } catch (const std::exception&) {
        return ErrorCode::InternalImporterFailure;
    } catch (...) {
        return ErrorCode::InternalImporterFailure;
    }
}

void StepAdapter::Reset() noexcept
{
    if (impl_) impl_->Reset();
}

std::uint64_t StepAdapter::DefinitionCount() const noexcept
{
    return impl_ ? impl_->definitions_.size() : 0;
}

std::uint64_t StepAdapter::OccurrenceCount() const noexcept
{
    return impl_ ? impl_->occurrences_.size() : 0;
}

std::uint64_t StepAdapter::MaterialCount() const noexcept
{
    return impl_ ? impl_->materials_.size() : 0;
}

std::uint64_t StepAdapter::InspectedTriangleCount() const noexcept
{
    return impl_ ? impl_->inspectedTriangles : 0;
}

std::uint64_t StepAdapter::ParseMilliseconds() const noexcept
{
    return impl_ ? impl_->parseMs : 0;
}

bool StepAdapter::ExternalDocumentRejected() const noexcept
{
    return impl_ && impl_->externalDocumentRejected;
}

bool StepAdapter::AdmissionRejected() const noexcept
{
    return impl_ && impl_->admissionRejected;
}

} // namespace preview3d::provider
