#pragma once

// Bounded FreeCAD Standard document interpretation. It consumes the already
// admitted ZIP entry catalog, extracts only the entries it needs into private
// memory, parses Document.xml/GuiDocument.xml with node/depth/size limits, and
// builds an ordinary XCAF document from the evaluated Part shape payloads.
//
// Supported subset (T09): one or more Part/PartDesign-layout objects with an
// evaluated `Part file="...brp"`, their placement/color/visibility, local
// links, link arrays, and nested App::Part/Assembly placements. FreeCAD
// features (sketches, history, expressions, FEM/CAM) are not evaluated; the
// stored Part shape is rendered as-is. External links and unknown objects are
// skipped with bounded warnings, never a whole-document failure.

#include "FcstdPreflight.h"

#include "model_core/Compatibility.h"
#include "model_core/ImportError.h"

#include <TDocStd_Document.hxx>

#include <cstdint>
#include <functional>
#include <span>

namespace step_host {

struct FcstdReadResult {
    model_core::ImportErrorCode errorCode = model_core::ImportErrorCode::None;
    std::uint32_t warningCount = 0;
    std::uint32_t visibleShapeCount = 0;
};

// Builds the XCAF document in place. `document` must already be a new XCAF
// document owned by the caller. The source bytes are the brokered read-only
// mapping; nothing is written to disk.
FcstdReadResult BuildFcstdDocument(const FcstdPackage& package,
                                   std::span<const std::byte> bytes,
                                   const Handle(TDocStd_Document)& document,
                                   const FcstdLimits& limits,
                                   model_core::CompatibilityReportBuilder& compatibility,
                                   const std::function<bool()>& isCancelled = {});

} // namespace step_host
