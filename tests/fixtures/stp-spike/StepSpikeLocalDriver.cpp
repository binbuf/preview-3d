#include "StepSpikeProtocol.h"
#include "StepXdeSpike.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: StepSpikeLocalDriver <file.stp>\n");
        return 1;
    }
    HANDLE source = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot open source\n");
        return 2;
    }
    HANDLE cancellation = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const SIZE_T size = sizeof(step_host::StepSpikeSection);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(size), nullptr);
    auto* header = static_cast<step_host::StepSpikeSection*>(
        MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size));
    if (!header) return 3;
    *header = step_host::StepSpikeSection{};
    header->sectionByteLength = size;
    header->sourceFileHandleValue = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(source));
    header->cancellationEventHandleValue = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(cancellation));

    const int result = step_host::RunStepSpike(*header);
    std::printf(
        "result=%d status=%u diag=%u roots=%u asm=%u shapes=%u defs=%u instances=%u reused=%u "
        "depth=%u solids=%u shells=%u faces=%u edges=%u colors=%u colored=%u transparent=%u "
        "tris=%llu verts=%llu meshed=%u mpu=%.9g doc_mpu=%.9g authored_mpu=%.9g unit=%s digest=0x%llx "
        "read_us=%llu transfer_us=%llu walk_us=%llu mesh_us=%llu total_us=%llu "
        "private=%llu peak=%llu msg=%s\n",
        result, static_cast<unsigned>(header->status), header->diagnosticCode,
        header->rootLabelCount, header->assemblyLabelCount, header->simpleShapeLabelCount,
        header->uniqueDefinitionCount, header->instanceCount, header->reusedDefinitionCount,
        header->maxObservedDepth, header->solidCount, header->shellCount, header->faceCount,
        header->edgeCount, header->colorCount, header->coloredLabelCount,
        header->transparentColorCount, static_cast<unsigned long long>(header->triangleCount),
        static_cast<unsigned long long>(header->extractedVertexCount),
        header->meshedDefinitionCount, header->metersPerUnit, header->documentMetersPerUnit,
        header->authoredMetersPerUnit, header->authoredLengthUnit,
        static_cast<unsigned long long>(header->semanticDigest),
        static_cast<unsigned long long>(header->readMicroseconds),
        static_cast<unsigned long long>(header->transferMicroseconds),
        static_cast<unsigned long long>(header->walkMicroseconds),
        static_cast<unsigned long long>(header->meshMicroseconds),
        static_cast<unsigned long long>(header->totalMicroseconds),
        static_cast<unsigned long long>(header->privateBytes),
        static_cast<unsigned long long>(header->peakWorkingSetBytes),
        header->diagnostic);
    return result;
}
