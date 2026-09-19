#define NOMINMAX

#include "StepXdeSpike.h"

#include "platform/Win32Handle.h"

#include <windows.h>
#include <psapi.h>

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
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <UnitsMethods.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ColorType.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <memory>
#include <ostream>
#include <span>
#include <streambuf>
#include <string>
#include <unordered_set>
#include <vector>

namespace step_host {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t Micros(Clock::time_point from, Clock::time_point to)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

bool Cancelled(const StepSpikeSection& out)
{
    const HANDLE event = reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(out.cancellationEventHandleValue));
    return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

void Memory(StepSpikeSection& out)
{
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info))) {
        out.privateBytes = info.PrivateUsage;
        out.peakWorkingSetBytes = info.PeakWorkingSetSize;
    }
}

// Product-owned seekable stream over the inherited read-only handle. The host
// has no path; every byte arrives through ReadFile on the broker-duplicated
// handle, which is the STEP-001 handle-only input contract.
class HandleStreamBuf final : public std::streambuf
{
public:
    HandleStreamBuf(HANDLE handle, std::uint64_t size)
        : handle_(handle), size_(size)
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

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode) override
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
        const auto want = static_cast<DWORD>(
            (std::min)(static_cast<std::uint64_t>(buffer_.size()), size_ - offset));
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

bool ReadAt(HANDLE handle, std::uint64_t offset, std::span<char> destination)
{
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    if (!ReadFile(handle, destination.data(), static_cast<DWORD>(destination.size()), &read, nullptr))
        return false;
    return read == destination.size();
}

// Minimal spike admission: verify the ISO 10303-21 physical-file envelope by
// bytes, never by extension. STEP-002 replaces this with the full bounded
// StepPart21Preflight lexical scanner.
bool AdmitPart21(HANDLE handle, std::uint64_t size)
{
    if (size < 32) return false;
    std::array<char, 4096> head{};
    const auto headLength = static_cast<std::size_t>((std::min)(size, head.size()));
    if (!ReadAt(handle, 0, std::span(head.data(), headLength))) return false;
    std::size_t offset = 0;
    if (headLength >= 3 && static_cast<unsigned char>(head[0]) == 0xef
        && static_cast<unsigned char>(head[1]) == 0xbb
        && static_cast<unsigned char>(head[2]) == 0xbf) {
        offset = 3;
    }
    while (offset < headLength && (head[offset] == ' ' || head[offset] == '\t'
        || head[offset] == '\r' || head[offset] == '\n')) {
        ++offset;
    }
    constexpr char signature[] = "ISO-10303-21;";
    if (headLength - offset < sizeof(signature) - 1) return false;
    if (std::memcmp(head.data() + offset, signature, sizeof(signature) - 1) != 0) return false;

    std::array<char, 64> tail{};
    const auto tailLength = static_cast<std::size_t>((std::min)(size, tail.size()));
    if (!ReadAt(handle, size - tailLength, std::span(tail.data(), tailLength))) return false;
    constexpr char terminator[] = "END-ISO-10303-21;";
    std::size_t end = tailLength;
    while (end > 0 && (tail[end - 1] == '\0' || tail[end - 1] == ' ' || tail[end - 1] == '\t'
        || tail[end - 1] == '\r' || tail[end - 1] == '\n')) {
        --end;
    }
    return end >= sizeof(terminator) - 1
        && std::memcmp(tail.data() + end - (sizeof(terminator) - 1), terminator,
                       sizeof(terminator) - 1) == 0;
}

struct Digest
{
    std::uint64_t value{14695981039346656037ull};
    void Scalar(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            value ^= bytes[index];
            value *= 1099511628211ull;
        }
    }
    template<class T> void Number(const T& item) { Scalar(&item, sizeof(item)); }
    void Real(double number)
    {
        const auto quantized = static_cast<std::int64_t>(std::llround(number * 1'000'000.0));
        Number(quantized);
    }
};

void CountTopology(const TopoDS_Shape& shape, StepSpikeSection& out)
{
    for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next())
        ++out.solidCount;
    for (TopExp_Explorer explorer(shape, TopAbs_SHELL); explorer.More(); explorer.Next())
        ++out.shellCount;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next())
        ++out.faceCount;
    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More(); explorer.Next())
        ++out.edgeCount;
}

void MeshShape(const TopoDS_Shape& shape, StepSpikeSection& out, Digest& digest)
{
    const bool relative = out.linearDeflection <= 0.0;
    const double deflection = relative ? 0.001 : out.linearDeflection;
    BRepMesh_IncrementalMesh mesher(shape, deflection, relative ? Standard_True : Standard_False,
                                    out.angularDeflection, Standard_True);
    mesher.Perform();
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        TopLoc_Location location;
        const Handle(Poly_Triangulation) triangulation =
            BRep_Tool::Triangulation(TopoDS::Face(explorer.Current()), location);
        if (triangulation.IsNull()) continue;
        out.triangleCount += static_cast<std::uint64_t>(triangulation->NbTriangles());
        out.extractedVertexCount += static_cast<std::uint64_t>(triangulation->NbNodes());
        for (Standard_Integer index = 1; index <= triangulation->NbNodes(); ++index) {
            const gp_Pnt point = triangulation->Node(index).Transformed(location.Transformation());
            const std::array<double, 3> coordinates{point.X(), point.Y(), point.Z()};
            digest.Number(coordinates);
            if (!std::isfinite(point.X()) || !std::isfinite(point.Y()) || !std::isfinite(point.Z()))
                continue;
        }
    }
    ++out.meshedDefinitionCount;
}

} // namespace

int RunStepSpike(StepSpikeSection& out)
{
    const auto start = Clock::now();
    InterlockedExchange(&out.state, 1);
    out.status = StepSpikeStatus::InvalidRequest;
    if (out.magic != kStepSpikeMagic || out.version != kStepSpikeVersion
        || !out.sourceFileHandleValue || out.sectionByteLength < sizeof(StepSpikeSection)) {
        return 2;
    }
    platform::Win32Handle source(reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(out.sourceFileHandleValue)));

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(source.get(), &size) || size.QuadPart <= 0) {
        out.status = StepSpikeStatus::ReadFailure;
        return 3;
    }
    if (!AdmitPart21(source.get(), static_cast<std::uint64_t>(size.QuadPart))) {
        out.status = StepSpikeStatus::PreflightFailure;
        strncpy_s(out.diagnostic, "not an ISO 10303-21 physical file", _TRUNCATE);
        return 4;
    }
    if (Cancelled(out)) {
        out.status = StepSpikeStatus::Cancelled;
        return 5;
    }

    Digest digest;
    try {
        Handle(TDocStd_Document) document;
        XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", document);
        if (document.IsNull()) {
            out.status = StepSpikeStatus::InternalFailure;
            return 6;
        }

        STEPCAFControl_Reader reader;
        reader.SetColorMode(Standard_True);
        reader.SetNameMode(Standard_True);
        reader.SetLayerMode(Standard_True);
        reader.SetPropsMode(Standard_False);
        reader.SetGDTMode(Standard_False);
        reader.SetViewMode(Standard_False);

        HandleStreamBuf buffer(source.get(), static_cast<std::uint64_t>(size.QuadPart));
        std::istream stream(&buffer);

        const auto readStart = Clock::now();
        const IFSelect_ReturnStatus readStatus = reader.ReadStream("preview3d.step", stream);
        const auto readEnd = Clock::now();
        out.readMicroseconds = Micros(readStart, readEnd);
        if (readStatus != IFSelect_RetDone) {
            out.status = StepSpikeStatus::ReadFailure;
            strncpy_s(out.diagnostic, "OCCT Part-21 stream read did not complete", _TRUNCATE);
            return 7;
        }
        InterlockedExchange(&out.state, 2);
        if (Cancelled(out)) {
            out.status = StepSpikeStatus::Cancelled;
            return 8;
        }

        const auto transferStart = Clock::now();
        const bool transferred = reader.Transfer(document);
        const auto transferEnd = Clock::now();
        out.transferMicroseconds = Micros(transferStart, transferEnd);
        if (!transferred) {
            out.status = StepSpikeStatus::TransferFailure;
            strncpy_s(out.diagnostic, "XDE transfer produced no accepted document", _TRUNCATE);
            return 9;
        }
        InterlockedExchange(&out.state, 3);

        const Handle(XCAFDoc_ShapeTool) shapeTool =
            XCAFDoc_DocumentTool::ShapeTool(document->Main());
        const Handle(XCAFDoc_ColorTool) colorTool =
            XCAFDoc_DocumentTool::ColorTool(document->Main());
        if (shapeTool.IsNull()) {
            out.status = StepSpikeStatus::NoGeometry;
            return 10;
        }

        const auto walkStart = Clock::now();
        TDF_LabelSequence roots;
        shapeTool->GetFreeShapes(roots);
        out.rootLabelCount = static_cast<std::uint32_t>(roots.Length());
        if (out.rootLabelCount == 0) {
            out.status = StepSpikeStatus::NoGeometry;
            return 11;
        }

        // Color palette count and transparency survey.
        if (!colorTool.IsNull()) {
            TDF_LabelSequence colors;
            colorTool->GetColors(colors);
            out.colorCount = static_cast<std::uint32_t>(colors.Length());
        }

        std::unordered_set<std::string> definitions;
        std::vector<std::pair<TDF_Label, std::uint32_t>> pending;
        for (Standard_Integer index = 1; index <= roots.Length(); ++index)
            pending.emplace_back(roots.Value(index), 1u);

        while (!pending.empty()) {
            if (Cancelled(out)) {
                out.status = StepSpikeStatus::Cancelled;
                return 12;
            }
            const auto [label, depth] = pending.back();
            pending.pop_back();
            if (depth > out.maxObservedDepth) out.maxObservedDepth = depth;
            if (depth > out.maxHierarchyDepth) {
                out.status = StepSpikeStatus::ResourceLimit;
                strncpy_s(out.diagnostic, "hierarchy depth exceeded spike ceiling", _TRUNCATE);
                return 13;
            }

            if (XCAFDoc_ShapeTool::IsAssembly(label)) {
                ++out.assemblyLabelCount;
                TDF_LabelSequence components;
                XCAFDoc_ShapeTool::GetComponents(label, components);
                for (Standard_Integer index = 1; index <= components.Length(); ++index) {
                    ++out.instanceCount;
                    const TDF_Label component = components.Value(index);
                    TDF_Label referred;
                    if (XCAFDoc_ShapeTool::GetReferredShape(component, referred)
                        && !referred.IsNull()) {
                        TCollection_AsciiString entry;
                        TDF_Tool::Entry(referred, entry);
                        definitions.insert(entry.ToCString());
                        pending.emplace_back(referred, depth + 1);
                    } else {
                        pending.emplace_back(component, depth + 1);
                    }
                }
                continue;
            }

            if (!XCAFDoc_ShapeTool::IsSimpleShape(label)
                && !XCAFDoc_ShapeTool::IsReference(label)) {
                continue;
            }
            ++out.simpleShapeLabelCount;
            const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
            if (shape.IsNull()) continue;

            if (!colorTool.IsNull()) {
                Quantity_ColorRGBA color;
                if (colorTool->GetColor(label, XCAFDoc_ColorSurf, color)
                    || colorTool->GetColor(label, XCAFDoc_ColorGen, color)
                    || colorTool->GetColor(label, XCAFDoc_ColorCurv, color)) {
                    ++out.coloredLabelCount;
                    if (color.Alpha() < 0.999f) ++out.transparentColorCount;
                    digest.Real(color.GetRGB().Red());
                    digest.Real(color.GetRGB().Green());
                    digest.Real(color.GetRGB().Blue());
                    digest.Real(color.Alpha());
                }
            }

            const auto meshStart = Clock::now();
            CountTopology(shape, out);
            MeshShape(shape, out, digest);
            out.meshMicroseconds += Micros(meshStart, Clock::now());
        }
        out.uniqueDefinitionCount = static_cast<std::uint32_t>(definitions.size());
        out.reusedDefinitionCount = out.instanceCount > out.uniqueDefinitionCount
            ? out.instanceCount - out.uniqueDefinitionCount : 0;
        out.walkMicroseconds = Micros(walkStart, Clock::now());
        InterlockedExchange(&out.state, 4);

        if (out.triangleCount == 0 && out.faceCount == 0) {
            out.status = StepSpikeStatus::NoGeometry;
            return 14;
        }

        double metersPerUnit = 0.0;
        if (XCAFDoc_DocumentTool::GetLengthUnit(document, metersPerUnit)
            && std::isfinite(metersPerUnit) && metersPerUnit > 0.0) {
            out.documentMetersPerUnit = metersPerUnit;
        }
        // LocalLengthUnit() is the Part-21 authored unit expressed against the
        // Cascade millimetre system unit (1.0 for mm, 25.4 for inch).
        const Handle(StepData_StepModel) model = reader.Reader().StepModel();
        if (!model.IsNull()) {
            const double authored = model->LocalLengthUnit() / 1000.0;
            if (std::isfinite(authored) && authored > 0.0) out.authoredMetersPerUnit = authored;
        }
        // The declared Part-21 unit name is independent of OCCT's normalized
        // system unit; it is the authoritative authored-unit label.
        TColStd_SequenceOfAsciiString lengthUnits, angleUnits, solidAngleUnits;
        reader.ChangeReader().FileUnits(lengthUnits, angleUnits, solidAngleUnits);
        if (lengthUnits.Length() > 0)
            strncpy_s(out.authoredLengthUnit, lengthUnits.Value(1).ToCString(), _TRUNCATE);
        out.metersPerUnit = out.authoredMetersPerUnit > 0.0
            ? out.authoredMetersPerUnit : out.documentMetersPerUnit;
        out.externFileCount = static_cast<std::uint32_t>(reader.ExternFiles().Extent());
        digest.Number(out.rootLabelCount);
        digest.Number(out.assemblyLabelCount);
        digest.Number(out.instanceCount);
        digest.Number(out.triangleCount);
        digest.Real(out.metersPerUnit);
        digest.Real(out.documentMetersPerUnit);
        out.semanticDigest = digest.value;
        Memory(out);
        out.totalMicroseconds = Micros(start, Clock::now());
        out.status = StepSpikeStatus::Success;
        InterlockedExchange(&out.state, 5);
        return 0;
    } catch (const std::exception&) {
        out.status = StepSpikeStatus::InternalFailure;
        strncpy_s(out.diagnostic, "OCCT raised a bounded C++ exception", _TRUNCATE);
        return 20;
    } catch (...) {
        out.status = StepSpikeStatus::InternalFailure;
        strncpy_s(out.diagnostic, "OCCT raised an unknown exception", _TRUNCATE);
        return 21;
    }
}

} // namespace step_host
