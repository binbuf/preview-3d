#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ThreeMfSpikeWorker.h"

#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <Bindings/Cpp/lib3mf_implicit.hpp>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <vector>

static_assert(LIB3MF_VERSION_MAJOR == 2 && LIB3MF_VERSION_MINOR == 5
              && LIB3MF_VERSION_MICRO == 0);

namespace import_worker {
namespace {

constexpr uint64_t kMaxVertices = 20'000'000;
constexpr uint64_t kMaxTriangles = 20'000'000;
constexpr uint64_t kMaxObjects = 50'000;
constexpr uint64_t kMaxPreviewTriangles = 262'144;
constexpr uint32_t kRadialSegments = 12;

struct MemorySample {
    uint64_t privateBytes = 0;
    uint64_t peakWorkingSetBytes = 0;
};

MemorySample SampleMemory()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) return {};
    return { static_cast<uint64_t>(counters.PrivateUsage),
             static_cast<uint64_t>(counters.PeakWorkingSetSize) };
}

uint64_t Microseconds(std::chrono::steady_clock::time_point begin)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count());
}

struct HashState {
    uint64_t value = 14695981039346656037ull;
    uint64_t bytes = 0;

    void Add(std::span<const std::byte> data)
    {
        for (const std::byte byte : data) {
            value ^= std::to_integer<uint8_t>(byte);
            value *= 1099511628211ull;
        }
        bytes += data.size();
    }

    template<class T> void Scalar(const T& valueToHash)
    {
        Add(std::as_bytes(std::span(&valueToHash, 1)));
    }
};

struct SourceContext {
    HANDLE file = nullptr;
    HANDLE cancellationEvent = nullptr;
    ThreeMfSpikePayloadHeader* header = nullptr;
    ThreeMfSpikeMode mode = ThreeMfSpikeMode::ReadAndInspect;
    BY_HANDLE_FILE_INFORMATION initialInfo{};
    uint64_t position = 0;
};

bool SameSource(const SourceContext& context)
{
    BY_HANDLE_FILE_INFORMATION current{};
    if (!GetFileInformationByHandle(context.file, &current)) return false;
    return current.dwVolumeSerialNumber == context.initialInfo.dwVolumeSerialNumber
        && current.nFileIndexHigh == context.initialInfo.nFileIndexHigh
        && current.nFileIndexLow == context.initialInfo.nFileIndexLow
        && current.nFileSizeHigh == context.initialInfo.nFileSizeHigh
        && current.nFileSizeLow == context.initialInfo.nFileSizeLow
        && CompareFileTime(&current.ftLastWriteTime, &context.initialInfo.ftLastWriteTime) == 0;
}

void ReadCallback(Lib3MF_uint64 destinationValue, Lib3MF_uint64 requested,
                  Lib3MF_pvoid userData)
{
    auto& context = *static_cast<SourceContext*>(userData);
    auto& header = *context.header;
    ++header.readCallbackCount;
    if (!SameSource(context)) {
        header.sourceChanged = 1;
        ++header.shortReadCount;
        if (requested != 0) {
            std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(destinationValue)), 0,
                        static_cast<size_t>((std::min)(requested, uint64_t(SIZE_MAX))));
        }
        return;
    }

    auto* destination = reinterpret_cast<std::byte*>(static_cast<uintptr_t>(destinationValue));
    uint64_t remaining = requested;
    if (context.mode == ThreeMfSpikeMode::ForceShortRead && remaining != 0) --remaining;
    uint64_t total = 0;
    while (total < remaining) {
        const DWORD chunk = static_cast<DWORD>((std::min)(remaining - total,
                                                          uint64_t(MAXDWORD)));
        DWORD read = 0;
        if (!ReadFile(context.file, destination + total, chunk, &read, nullptr) || read == 0) break;
        total += read;
        if (read != chunk) break;
    }
    if (total < requested) {
        std::memset(destination + total, 0, static_cast<size_t>(requested - total));
        ++header.shortReadCount;
    }
    header.sourceBytesRead += total;
    context.position += total;
}

void SeekCallback(Lib3MF_uint64 position, Lib3MF_pvoid userData)
{
    auto& context = *static_cast<SourceContext*>(userData);
    auto& header = *context.header;
    ++header.seekCallbackCount;
    const uint64_t distance = position > context.position ? position - context.position
                                                          : context.position - position;
    if (position < context.position || distance >= 1024 * 1024) ++header.sparseSeekCount;
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(position);
    if (!SetFilePointerEx(context.file, target, nullptr, FILE_BEGIN)) {
        ++header.shortReadCount;
        return;
    }
    context.position = position;
}

void ProgressCallback(bool* abort, Lib3MF_double, Lib3MF::eProgressIdentifier identifier,
                      Lib3MF_pvoid userData)
{
    auto& context = *static_cast<SourceContext*>(userData);
    auto& header = *context.header;
    ++header.progressCallbackCount;
    const auto raw = static_cast<uint32_t>(identifier);
    if (raw < 32) header.progressIdentifierMask |= (1u << raw);
    const bool targetPhase = context.mode == ThreeMfSpikeMode::CancelFromProgress
        && header.reserved != 0 && header.reserved == raw + 1;
    *abort = targetPhase || WaitForSingleObject(context.cancellationEvent, 0) == WAIT_OBJECT_0;
}

struct Vec3 {
    double x = 0;
    double y = 0;
    double z = 0;
};

Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator*(Vec3 a, double scale) { return { a.x * scale, a.y * scale, a.z * scale }; }
double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}

bool Normalize(Vec3& value)
{
    const double length = std::sqrt(Dot(value, value));
    if (!std::isfinite(length) || length <= 1e-12) return false;
    value = value * (1.0 / length);
    return true;
}

class PreviewMesh {
public:
    bool AddTriangle(Vec3 a, Vec3 b, Vec3 c)
    {
        if (triangleCount_ >= kMaxPreviewTriangles) return false;
        for (const Vec3 point : { a, b, c }) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                return false;
            vertices_.push_back(static_cast<float>(point.x));
            vertices_.push_back(static_cast<float>(point.y));
            vertices_.push_back(static_cast<float>(point.z));
        }
        ++triangleCount_;
        return true;
    }

    bool AddSphere(Vec3 center, double radius)
    {
        constexpr uint32_t latitudes = 8;
        const Vec3 north{ center.x, center.y, center.z + radius };
        const Vec3 south{ center.x, center.y, center.z - radius };
        std::array<Vec3, kRadialSegments> previous{};
        for (uint32_t latitude = 1; latitude < latitudes; ++latitude) {
            const double theta = std::numbers::pi * latitude / latitudes;
            std::array<Vec3, kRadialSegments> ring{};
            for (uint32_t segment = 0; segment < kRadialSegments; ++segment) {
                const double phi = 2.0 * std::numbers::pi * segment / kRadialSegments;
                ring[segment] = { center.x + radius * std::sin(theta) * std::cos(phi),
                                  center.y + radius * std::sin(theta) * std::sin(phi),
                                  center.z + radius * std::cos(theta) };
            }
            if (latitude == 1) {
                for (uint32_t segment = 0; segment < kRadialSegments; ++segment)
                    if (!AddTriangle(north, ring[segment], ring[(segment + 1) % kRadialSegments]))
                        return false;
            } else {
                for (uint32_t segment = 0; segment < kRadialSegments; ++segment) {
                    const uint32_t next = (segment + 1) % kRadialSegments;
                    if (!AddTriangle(previous[segment], ring[segment], ring[next])
                        || !AddTriangle(previous[segment], ring[next], previous[next])) return false;
                }
            }
            previous = ring;
        }
        for (uint32_t segment = 0; segment < kRadialSegments; ++segment) {
            if (!AddTriangle(previous[(segment + 1) % kRadialSegments], previous[segment], south))
                return false;
        }
        return true;
    }

    bool AddBeam(Vec3 begin, Vec3 end, const Lib3MF::sBeam& beam)
    {
        Vec3 axis = end - begin;
        if (!Normalize(axis)) return false;
        Vec3 reference = std::abs(axis.z) < 0.9 ? Vec3{ 0, 0, 1 } : Vec3{ 0, 1, 0 };
        Vec3 tangent = Cross(axis, reference);
        if (!Normalize(tangent)) return false;
        const Vec3 bitangent = Cross(axis, tangent);
        std::array<Vec3, kRadialSegments> first{};
        std::array<Vec3, kRadialSegments> second{};
        for (uint32_t segment = 0; segment < kRadialSegments; ++segment) {
            const double angle = 2.0 * std::numbers::pi * segment / kRadialSegments;
            const Vec3 radial = tangent * std::cos(angle) + bitangent * std::sin(angle);
            first[segment] = begin + radial * beam.m_Radii[0];
            second[segment] = end + radial * beam.m_Radii[1];
        }
        for (uint32_t segment = 0; segment < kRadialSegments; ++segment) {
            const uint32_t next = (segment + 1) % kRadialSegments;
            if (!AddTriangle(first[segment], second[segment], second[next])
                || !AddTriangle(first[segment], second[next], first[next])) return false;
        }
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
            const auto mode = beam.m_CapModes[endpoint];
            const Vec3 center = endpoint == 0 ? begin : end;
            const double radius = beam.m_Radii[endpoint];
            if (mode == Lib3MF::eBeamLatticeCapMode::Butt) {
                const auto& ring = endpoint == 0 ? first : second;
                for (uint32_t segment = 0; segment < kRadialSegments; ++segment) {
                    const uint32_t next = (segment + 1) % kRadialSegments;
                    if (endpoint == 0) {
                        if (!AddTriangle(center, ring[next], ring[segment])) return false;
                    } else if (!AddTriangle(center, ring[segment], ring[next])) return false;
                }
            } else {
                // The spike deliberately overdraws a sphere for both spherical cap modes.
                // Product tessellation must clip a hemisphere at the beam end plane.
                if (!AddSphere(center, radius)) return false;
            }
        }
        return true;
    }

    uint32_t TriangleCount() const { return static_cast<uint32_t>(triangleCount_); }

    uint64_t Hash() const
    {
        HashState hash;
        hash.Add(std::as_bytes(std::span(vertices_)));
        return hash.value;
    }

private:
    std::vector<float> vertices_;
    uint64_t triangleCount_ = 0;
};

bool AddCount(uint64_t value, uint64_t limit, uint32_t& destination)
{
    if (value > limit || uint64_t(destination) + value > limit
        || uint64_t(destination) + value > uint64_t(UINT32_MAX)) return false;
    destination += static_cast<uint32_t>(value);
    return true;
}

bool InspectModel(const Lib3MF::PModel& model, ThreeMfSpikePayloadHeader& header,
                  HANDLE cancellationEvent, ThreeMfSpikeMode mode, bool& cancelled)
{
    if (!AddCount(model->GetObjects()->Count(), kMaxObjects, header.objectCount)
        || !AddCount(model->GetMeshObjects()->Count(), kMaxObjects, header.meshCount)
        || !AddCount(model->GetComponentsObjects()->Count(), kMaxObjects,
                     header.componentObjectCount)
        || !AddCount(model->GetBuildItems()->Count(), kMaxObjects, header.buildItemCount)
        || !AddCount(model->GetTexture2Ds()->Count(), kMaxObjects, header.textureCount)
        || !AddCount(model->GetBaseMaterialGroups()->Count(), UINT32_MAX,
                     header.materialResourceCount)
        || !AddCount(model->GetColorGroups()->Count(), UINT32_MAX,
                     header.materialResourceCount)
        || !AddCount(model->GetTexture2DGroups()->Count(), UINT32_MAX,
                     header.materialResourceCount)
        || !AddCount(model->GetCompositeMaterials()->Count(), UINT32_MAX,
                     header.materialResourceCount)
        || !AddCount(model->GetMultiPropertyGroups()->Count(), UINT32_MAX,
                     header.materialResourceCount)) return false;

    HashState normalized;
    PreviewMesh preview;
    auto meshes = model->GetMeshObjects();
    while (meshes->MoveNext()) {
        if (WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
            cancelled = true;
            return false;
        }
        const auto mesh = meshes->GetCurrentMeshObject();
        std::vector<Lib3MF::sPosition> vertices;
        std::vector<Lib3MF::sTriangle> triangles;
        mesh->GetVertices(vertices);
        if (mode == ThreeMfSpikeMode::CancelDuringExtraction)
            SetEvent(cancellationEvent);
        if (WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
            cancelled = true;
            return false;
        }
        mesh->GetTriangleIndices(triangles);
        if (!AddCount(vertices.size(), kMaxVertices, header.vertexCount)
            || !AddCount(triangles.size(), kMaxTriangles, header.triangleCount)) return false;
        for (const auto& vertex : vertices) {
            for (const float coordinate : vertex.m_Coordinates)
                if (!std::isfinite(coordinate)) return false;
        }
        for (const auto& triangle : triangles) {
            for (const uint32_t index : triangle.m_Indices)
                if (index >= vertices.size()) return false;
        }
        normalized.Add(std::as_bytes(std::span(vertices)));
        normalized.Add(std::as_bytes(std::span(triangles)));

        const auto lattice = mesh->BeamLattice();
        const uint32_t beamCount = lattice->GetBeamCount();
        const uint32_t ballCount = lattice->GetBallCount();
        if (!AddCount(beamCount, 100'000, header.beamCount)
            || !AddCount(ballCount, 100'000, header.ballCount)) return false;
        Lib3MF_uint32 representation = 0;
        if (lattice->GetRepresentation(representation)) {
            ++header.representationCount;
            const auto representationMesh = model->GetMeshObjectByID(representation);
            if (!representationMesh || representationMesh->GetTriangleCount() > kMaxPreviewTriangles)
                return false;
        }
        Lib3MF::eBeamLatticeClipMode clipMode{};
        Lib3MF_uint32 clippingResource = 0;
        lattice->GetClipping(clipMode, clippingResource);
        (void)clippingResource;
        for (uint32_t index = 0; index < beamCount; ++index) {
            if (WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
                cancelled = true;
                return false;
            }
            const auto beam = lattice->GetBeam(index);
            if (mode == ThreeMfSpikeMode::CancelDuringLattice && index == 0)
                SetEvent(cancellationEvent);
            if (WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
                cancelled = true;
                return false;
            }
            if (beam.m_Indices[0] >= vertices.size() || beam.m_Indices[1] >= vertices.size()
                || !std::isfinite(beam.m_Radii[0]) || !std::isfinite(beam.m_Radii[1])
                || beam.m_Radii[0] <= 0 || beam.m_Radii[1] <= 0) return false;
            normalized.Scalar(beam);
            if (clipMode == Lib3MF::eBeamLatticeClipMode::NoClipMode
                && beam.m_Radii[0] == beam.m_Radii[1]
                && beam.m_CapModes[0] == Lib3MF::eBeamLatticeCapMode::Sphere
                && beam.m_CapModes[1] == Lib3MF::eBeamLatticeCapMode::Sphere) {
                ++header.instancingEligibleBeamCount;
            }
            const auto& first = vertices[beam.m_Indices[0]];
            const auto& second = vertices[beam.m_Indices[1]];
            if (!preview.AddBeam({ first.m_Coordinates[0], first.m_Coordinates[1],
                                   first.m_Coordinates[2] },
                                 { second.m_Coordinates[0], second.m_Coordinates[1],
                                   second.m_Coordinates[2] }, beam)) return false;
        }
        for (uint32_t index = 0; index < ballCount; ++index) {
            if (WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0) {
                cancelled = true;
                return false;
            }
            const auto ball = lattice->GetBall(index);
            if (ball.m_Index >= vertices.size() || !std::isfinite(ball.m_Radius)
                || ball.m_Radius <= 0) return false;
            normalized.Scalar(ball);
            const auto& center = vertices[ball.m_Index];
            if (!preview.AddSphere({ center.m_Coordinates[0], center.m_Coordinates[1],
                                     center.m_Coordinates[2] }, ball.m_Radius)) return false;
        }
    }
    header.normalizedHash = normalized.value;
    header.normalizedBytes = normalized.bytes;
    header.tessellatedTriangleCount = preview.TriangleCount();
    header.previewHash = preview.Hash();
    return true;
}

void ClearResults(ThreeMfSpikePayloadHeader& header)
{
    const uint64_t magic = header.magic;
    const uint64_t sourceFileHandleValue = header.sourceFileHandleValue;
    const uint64_t sourceByteLength = header.sourceByteLength;
    const uint64_t cancellationEventHandleValue = header.cancellationEventHandleValue;
    const uint32_t mode = header.mode;
    const uint32_t reserved = header.reserved;
    std::memset(&header, 0, sizeof(header));
    header.magic = magic;
    header.sourceFileHandleValue = sourceFileHandleValue;
    header.sourceByteLength = sourceByteLength;
    header.cancellationEventHandleValue = cancellationEventHandleValue;
    header.mode = mode;
    header.reserved = reserved;
}

void RunProbe(ThreeMfSpikePayloadHeader& header, HANDLE sourceFile, HANDLE cancellationEvent)
{
    ClearResults(header);
    header.status = static_cast<uint32_t>(ThreeMfSpikeStatus::Pending);
    SourceContext context{};
    context.file = sourceFile;
    context.cancellationEvent = cancellationEvent;
    context.header = &header;
    context.mode = static_cast<ThreeMfSpikeMode>(header.mode);
    if (!GetFileInformationByHandle(sourceFile, &context.initialInfo)) {
        header.status = static_cast<uint32_t>(ThreeMfSpikeStatus::IoFailure);
        return;
    }
    const uint64_t actualLength = (uint64_t(context.initialInfo.nFileSizeHigh) << 32)
        | context.initialInfo.nFileSizeLow;
    if (actualLength != header.sourceByteLength) {
        header.sourceChanged = 1;
        header.status = static_cast<uint32_t>(ThreeMfSpikeStatus::IoFailure);
        return;
    }
    if (context.mode == ThreeMfSpikeMode::SimulateSourceChange)
        context.initialInfo.ftLastWriteTime.dwLowDateTime ^= 1u;

    const auto before = SampleMemory();
    header.privateBytesBefore = before.privateBytes;
    header.peakWorkingSetBytes = before.peakWorkingSetBytes;
    try {
        auto wrapper = Lib3MF::CWrapper::loadLibrary();
        wrapper->GetLibraryVersion(header.versionMajor, header.versionMinor, header.versionMicro);
        auto model = wrapper->CreateModel();
        auto reader = model->QueryReader("3mf");
        reader->SetStrictModeActive(true);
        reader->SetProgressCallback(ProgressCallback, &context);
        const auto loadStart = std::chrono::steady_clock::now();
        InterlockedExchange(&header.state, 1);
        reader->ReadFromCallback(ReadCallback, header.sourceByteLength, SeekCallback, &context);
        header.loadMicroseconds = Microseconds(loadStart);
        header.warningCount = reader->GetWarningCount();
        const auto afterLoad = SampleMemory();
        header.privateBytesAfterLoad = afterLoad.privateBytes;
        header.peakWorkingSetBytes = (std::max)(header.peakWorkingSetBytes,
                                               afterLoad.peakWorkingSetBytes);
        if (header.shortReadCount != 0 || header.sourceChanged != 0) {
            header.status = static_cast<uint32_t>(ThreeMfSpikeStatus::IoFailure);
            return;
        }
        InterlockedExchange(&header.state, 2);
        if (context.mode == ThreeMfSpikeMode::HoldAfterLoad) {
            Sleep(5000);
            return;
        }
        const auto extractStart = std::chrono::steady_clock::now();
        bool cancelled = false;
        if (!InspectModel(model, header, cancellationEvent, context.mode, cancelled)) {
            header.extractMicroseconds = Microseconds(extractStart);
            header.status = static_cast<uint32_t>(cancelled
                ? ThreeMfSpikeStatus::Cancelled : ThreeMfSpikeStatus::ResourceLimit);
            return;
        }
        header.extractMicroseconds = Microseconds(extractStart);
        const auto afterExtract = SampleMemory();
        header.privateBytesAfterExtract = afterExtract.privateBytes;
        header.peakWorkingSetBytes = (std::max)(header.peakWorkingSetBytes,
                                               afterExtract.peakWorkingSetBytes);
        header.status = static_cast<uint32_t>(ThreeMfSpikeStatus::Success);
        InterlockedExchange(&header.state, 3);
    } catch (const Lib3MF::ELib3MFException& error) {
        header.libraryError = error.getErrorCode();
        header.status = static_cast<uint32_t>(error.getErrorCode() == LIB3MF_ERROR_CALCULATIONABORTED
            ? ThreeMfSpikeStatus::Cancelled
            : (header.shortReadCount != 0 || header.sourceChanged != 0
                ? ThreeMfSpikeStatus::IoFailure : ThreeMfSpikeStatus::Malformed));
    } catch (const std::bad_alloc&) {
        header.status = static_cast<uint32_t>(ThreeMfSpikeStatus::ResourceLimit);
    } catch (...) {
        header.status = static_cast<uint32_t>(ThreeMfSpikeStatus::InternalError);
    }
}

bool HandleRequest(HANDLE stdOut, const model_core::StartGenerationRequest& request)
{
    if (request.sectionByteCapacity < sizeof(ThreeMfSpikePayloadHeader)
        || request.sectionByteCapacity > uint64_t((std::numeric_limits<SIZE_T>::max)())) return false;
    platform::Win32Handle section(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.sectionHandleValue)));
    auto view = platform::MappedView::Map(section.get(), FILE_MAP_READ | FILE_MAP_WRITE,
                                          static_cast<SIZE_T>(request.sectionByteCapacity));
    if (!view) return false;
    auto* header = reinterpret_cast<ThreeMfSpikePayloadHeader*>(view.bytes().data());
    if (header->magic != kThreeMfSpikePayloadMagic
        || header->sourceByteLength == 0
        || header->sourceByteLength > 2ull * 1024 * 1024 * 1024
        || header->mode > static_cast<uint32_t>(ThreeMfSpikeMode::SimulateSourceChange))
        return false;
    platform::Win32Handle sourceFile(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(header->sourceFileHandleValue)));
    platform::Win32Handle cancellationEvent(reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(header->cancellationEventHandleValue)));
    if (!sourceFile || !cancellationEvent) return false;
    RunProbe(*header, sourceFile.get(), cancellationEvent.get());
    model_core::ChunksReadyNotice notice{};
    notice.generationId = request.generationId;
    notice.sectionBytesWritten = sizeof(*header);
    return model_core::WriteControlMessage(stdOut, model_core::ControlOpcode::ChunksReady,
                                           &notice, sizeof(notice));
}

} // namespace

int RunThreeMfSpikePoolMode()
{
    HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    for (;;) {
        auto message = model_core::ReadControlMessage(stdIn);
        if (!message) return 1;
        if (message->header.opcode == uint32_t(model_core::ControlOpcode::Shutdown)) return 0;
        if (message->header.opcode != uint32_t(model_core::ControlOpcode::StartGeneration)
            || message->payload.size() != sizeof(model_core::StartGenerationRequest)) return 1;
        model_core::StartGenerationRequest request{};
        std::memcpy(&request, message->payload.data(), sizeof(request));
        if (request.sceneVariant != kThreeMfSpikeSceneVariant || !HandleRequest(stdOut, request))
            return 1;
    }
}

} // namespace import_worker
