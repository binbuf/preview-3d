#pragma once

// T34 STEP/STP family adapter (ADR-0002: a separately built, explicitly
// limited OCCT STEP/XDE/tessellation adapter linked only into this DLL).
//
// Renders self-contained `.step`/`.stp` visual geometry selected only by the
// routed `Family::Step` CLSID `{6EE961AC-AC3B-4958-A898-E30523FEE79D}` -- never
// by extension -- through the frozen `IFamilyAdapter` lifecycle
// (FamilyAdapter.h):
//
//   * the adapter consumes only the bounded `BoundedSource` (T12), never a path,
//     sidecar, network endpoint or child process; it launches no
//     `Preview3DStepHost.exe`;
//   * product-owned Part-21 admission (`StepPart21Preflight`, the same source
//     the STEP host compiles) runs before any OCCT call and rejects external
//     documents (`FILE_POPULATION`/`DOCUMENT_FILE`), unsupported encodings and
//     over-budget input;
//   * the pinned OCCT 7.8 XDE/STEP reader is fed a bounded, seekable stream over
//     the Shell stream, under the stricter provider ceilings (256 MiB stream,
//     192 MiB accounted scratch, a reduced triangle cap);
//   * a fixed low-detail, deterministic tessellation/sample policy is used and
//     the product CPU rasterizer draws the result. Bounded shape colors and the
//     full assembly placement are preserved where possible; unsupported or
//     over-budget input safely returns a typed failure so Explorer uses its
//     normal icon.
//
// The translation unit is PCH-free and free of GDI/COM so `Tests.Unit.exe` and
// `Tests.ProviderHost.exe` compile the exact source the DLL links. OCCT headers
// stay in the .cpp: only `preview3d::provider`/`model_core` types cross this
// boundary.

#include "FamilyAdapter.h"
#include "ProviderTypes.h"

#include <cstdint>
#include <memory>

namespace preview3d::provider {

class StepAdapter final : public IFamilyAdapter {
public:
    StepAdapter() noexcept;
    ~StepAdapter() noexcept override;

    StepAdapter(const StepAdapter&) = delete;
    StepAdapter& operator=(const StepAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

    // Diagnostics/test hooks. None changes the product decision.
    std::uint64_t DefinitionCount() const noexcept;
    std::uint64_t OccurrenceCount() const noexcept;
    std::uint64_t MaterialCount() const noexcept;
    std::uint64_t InspectedTriangleCount() const noexcept;
    std::uint64_t ParseMilliseconds() const noexcept;
    bool ExternalDocumentRejected() const noexcept;
    bool AdmissionRejected() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace preview3d::provider