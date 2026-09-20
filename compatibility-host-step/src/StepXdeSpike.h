#pragma once

// STEP-001 test-only OCCT/XDE stream adapter. Reads accepted Part-21 bytes
// exclusively through a product-owned seekable std::streambuf over the
// inherited read-only source handle, transfers through STEPCAFControl_Reader
// into an XDE document, walks the assembly/definition graph, and tessellates
// with BRepMesh_IncrementalMesh. No path, directory, URL, or child process is
// ever opened. This is spike plumbing, not the production scene adapter.

#include "StepSpikeProtocol.h"

#include <cstddef>
#include <cstdint>

namespace step_host {

// Reads the source handle into the XDE reader and fills `out`. Returns 0 on a
// successful bounded proof and non-zero on an internal (non-OCCT-message)
// failure. OCCT diagnostics never cross this boundary.
int RunStepSpike(StepSpikeSection& out);

} // namespace step_host
