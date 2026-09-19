// Preview3DStepHost.exe -- dedicated, zero-capability AppContainer import host
// for the bounded static STEP/STP preview subset. It receives an already-open
// read-only source handle (never a path), performs product-owned Part-21
// admission, and emits only the normalized protocol-v10 wire contract. The
// trusted viewer and the general import worker never link or load this host's
// OCCT closure.

#include "StepHostImport.h"
#include "StepPart21Preflight.h"
#include "StepSpikeProtocol.h"
#include "StepXdeSpike.h"

#include "ContainmentProbes.h"
#include "ChunkBatchSink.h"
#include "model_core/Checksum.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/ImportError.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

bool ReadExact(HANDLE handle, void* buffer, DWORD length)
{
    auto* bytes = static_cast<std::byte*>(buffer);
    DWORD total = 0;
    while (total < length) {
        DWORD read = 0;
        if (!ReadFile(handle, bytes + total, length - total, &read, nullptr) || !read) return false;
        total += read;
    }
    return true;
}

bool WriteExact(HANDLE handle, const void* buffer, DWORD length)
{
    const auto* bytes = static_cast<const std::byte*>(buffer);
    DWORD total = 0;
    while (total < length) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes + total, length - total, &written, nullptr) || !written)
            return false;
        total += written;
    }
    return true;
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

// Fixed, build-independent hardening: system + the explicitly added
// executable directory only, and every OCCT ambient configuration escape
// hatch cleared before any resource or plug-in could be consulted. STEP-003
// installs the signed OCCT payload here and relies on this configuration.
bool HardenProcessDiscovery(const std::filesystem::path& directory)
{
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS))
        return false;
    if (!AddDllDirectory(directory.c_str())) return false;
    constexpr const wchar_t* variables[] = {
        L"CSF_OCCTResourcePath", L"CSF_PluginPath", L"CSF_UnitsLexicon",
        L"CSF_DefaultUnit", L"CSF_UnitsDefinition", L"CSF_IGESDefaults",
        L"CSF_STEPDefaults", L"CSF_XCAFDefaults", L"CSF_DrawPluginPath",
        L"CSF_MDTVTexturesDirectory", L"CSF_ShadersDirectory",
        L"CSF_GraphicShr", L"MMGT_OPT", L"PATH"
    };
    for (const auto variable : variables) SetEnvironmentVariableW(variable, nullptr);
    return true;
}

bool SendError(uint64_t generationId, model_core::ImportErrorCode code)
{
    model_core::GenerationErrorNotice notice{};
    notice.generationId = generationId;
    notice.errorCode = static_cast<uint32_t>(code);
    return model_core::WriteControlMessage(
        GetStdHandle(STD_OUTPUT_HANDLE), model_core::ControlOpcode::GenerationError,
        &notice, sizeof(notice));
}

bool SendChunksReady(uint64_t generationId, uint32_t chunkCount, uint64_t sectionBytesWritten)
{
    model_core::ChunksReadyNotice notice{};
    notice.generationId = generationId;
    notice.chunkCount = chunkCount;
    notice.sectionBytesWritten = sectionBytesWritten;
    return model_core::WriteControlMessage(
        GetStdHandle(STD_OUTPUT_HANDLE), model_core::ControlOpcode::ChunksReady,
        &notice, sizeof(notice));
}

// STEP-005: bounded phase/progress signal. Best-effort -- a failed write means
// the broker has gone away, and the terminal reply path reports that anyway.
void SendStepProgress(uint64_t generationId, const step_host::StepProgressEvent& event)
{
    model_core::StepProgressNotice notice{};
    notice.generationId = generationId;
    notice.phase = event.phase;
    notice.definitionsMeshed = event.definitionsMeshed;
    notice.definitionTotal = event.definitionTotal;
    notice.preflightBytes = event.preflightBytes;
    notice.phaseMilliseconds = event.phaseMilliseconds;
    notice.totalMilliseconds = event.totalMilliseconds;
    model_core::WriteControlMessage(GetStdHandle(STD_OUTPUT_HANDLE),
                                    model_core::ControlOpcode::StepProgress,
                                    &notice, sizeof(notice));
}

enum class PoolMode { Normal, Crash, Hang, ConsumeMemory, StaleReply, WrongFormat, UnknownError };

// Test-only fault: rewrite the validated synthetic scene's source format so
// the broker's closed expected-format rule must reject it.
void SpoofSceneFormat(std::span<std::byte> section, model_core::SourceFormatId format)
{
    if (section.size() < sizeof(model_core::SectionHeader)) return;
    auto* header = reinterpret_cast<model_core::SectionHeader*>(section.data());
    header->scene.format = format;
    header->sectionChecksum = model_core::WireChecksum64(
        section.subspan(sizeof(model_core::SectionHeader),
                        static_cast<std::size_t>(header->sectionLength - sizeof(model_core::SectionHeader))));
}

int RunProductionPool(PoolMode mode)
{
    for (;;) {
        const auto received = model_core::ReadControlMessage(GetStdHandle(STD_INPUT_HANDLE));
        if (!received) return 73;
        const auto opcode = static_cast<model_core::ControlOpcode>(received->header.opcode);
        if (opcode == model_core::ControlOpcode::Shutdown && received->payload.empty())
            return 0;
        if (opcode != model_core::ControlOpcode::StartStepImportFromFile
            || received->payload.size() != sizeof(model_core::ParseStepFileRequest))
            return 74;

        model_core::ParseStepFileRequest request{};
        std::memcpy(&request, received->payload.data(), sizeof(request));

        if (mode == PoolMode::Crash) { RaiseFailFastException(nullptr, nullptr, 0); return 79; }
        if (mode == PoolMode::Hang) { Sleep(INFINITE); return 80; }
        if (mode == PoolMode::ConsumeMemory) {
            for (;;) {
                void* allocation = VirtualAlloc(nullptr, 16 * 1024 * 1024,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (!allocation) break;
                std::memset(allocation, 0x5a, 16 * 1024 * 1024);
            }
            return 81;
        }
        if (mode == PoolMode::StaleReply) {
            SendError(request.generationId + 1, model_core::ImportErrorCode::MalformedData);
            continue;
        }
        if (mode == PoolMode::UnknownError) {
            SendError(request.generationId, static_cast<model_core::ImportErrorCode>(0xFFFFFFFFu));
            continue;
        }

        const bool malformedRequest = !request.generationId || !request.sourceFileHandleValue
            || !request.sectionHandleValue || !request.cancellationEventHandleValue
            || request.sectionByteCapacity < sizeof(model_core::SectionHeader)
            || !request.maxChunkCount
            || (request.requestFlags & ~(model_core::kImportRequestDetailService
                | model_core::kImportRequestCoarseProxy
                | model_core::kImportRequestStepForceSerialForTesting));
        if (malformedRequest) {
            if (!SendError(request.generationId, model_core::ImportErrorCode::ImportProtocolViolation))
                return 75;
            continue;
        }

        platform::Win32Handle source(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(request.sourceFileHandleValue)));
        platform::Win32Handle cancellation(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(request.cancellationEventHandleValue)));
        platform::Win32Handle output(reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(request.sectionHandleValue)));
        auto outputView = platform::MappedView::Map(
            output.get(), FILE_MAP_READ | FILE_MAP_WRITE,
            static_cast<SIZE_T>(request.sectionByteCapacity));
        if (!outputView) {
            if (!SendError(request.generationId, model_core::ImportErrorCode::InternalImporterFailure))
                return 76;
            continue;
        }

        import_worker::ChunkBatchSink batchSink(
            GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
            request.generationId, request.requestFlags, cancellation.get());
        const auto result = step_host::RunStepHostImport(
            request, outputView.bytes(), source.get(), cancellation.get(),
            [&batchSink](std::uint32_t chunkCount, std::uint64_t sectionBytesWritten) {
                return batchSink.PublishBatch(chunkCount, sectionBytesWritten);
            },
            [&request](const step_host::StepProgressEvent& event) {
                SendStepProgress(request.generationId, event);
            });
        if (result.errorCode != model_core::ImportErrorCode::None) {
            if (!SendError(request.generationId, result.errorCode)) return 78;
            continue;
        }
        if (mode == PoolMode::WrongFormat)
            SpoofSceneFormat(outputView.bytes(), model_core::SourceFormatId::ThreeMf);
        if (!SendChunksReady(request.generationId, result.chunkCount, result.sectionBytesWritten))
            return 78;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const auto directory = ExecutableDirectory();
    if (directory.empty() || !HardenProcessDiscovery(directory)) return 65;

    if (argc == 2) {
        const std::wstring_view mode(argv[1]);
        if (mode == L"--pool") return RunProductionPool(PoolMode::Normal);
        if (mode == L"--pool-crash") return RunProductionPool(PoolMode::Crash);
        if (mode == L"--pool-hang") return RunProductionPool(PoolMode::Hang);
        if (mode == L"--pool-overallocate") return RunProductionPool(PoolMode::ConsumeMemory);
        if (mode == L"--pool-stale") return RunProductionPool(PoolMode::StaleReply);
        if (mode == L"--pool-wrong-format") return RunProductionPool(PoolMode::WrongFormat);
        if (mode == L"--pool-unknown-error") return RunProductionPool(PoolMode::UnknownError);
    }

    if (argc == 4 && std::wstring_view(argv[1]) == L"--probes") {
        // Test-only containment proof: the same canary/network/child-process
        // probes the general worker exposes, run under this host's real
        // zero-capability AppContainer identity.
        const std::wstring canary(argv[2]);
        const unsigned short port = static_cast<unsigned short>(_wtoi(argv[3]));
        import_worker::RunFilesystemEscapeProbe(canary.c_str());
        import_worker::RunNetworkEscapeProbe(port);
        import_worker::RunProcessSpawnEscapeProbe();
        import_worker::ReportDone();
        return 0;
    }

    // Test-only STEP-001 spike route retained for the OCCT/XDE feasibility
    // harness. It is not part of the production pool contract.
    using namespace step_host;
    if (argc != 4 || std::wstring_view(argv[1]) != L"--step-001-spike") return 64;

    wchar_t* end = nullptr;
    const auto handleValue = _wcstoui64(argv[2], &end, 10);
    if (!end || *end || !handleValue) return 65;
    end = nullptr;
    const auto capacity = _wcstoui64(argv[3], &end, 10);
    if (!end || *end || capacity < sizeof(StepSpikeSection) || capacity > kStepSpikeMaxSectionBytes)
        return 66;

    auto view = platform::MappedView::Map(
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handleValue)),
        FILE_MAP_READ | FILE_MAP_WRITE, static_cast<std::size_t>(capacity));
    if (!view) return 68;

    StepSpikeControl control{};
    if (!ReadExact(GetStdHandle(STD_INPUT_HANDLE), &control, sizeof(control))
        || control.magic != kStepSpikeMagic || control.version != kStepSpikeVersion) return 69;

    auto& header = *reinterpret_cast<StepSpikeSection*>(view.bytes().data());
    const int result = RunStepSpike(header);
    WriteExact(GetStdHandle(STD_OUTPUT_HANDLE), &control, sizeof(control));
    return result;
}
