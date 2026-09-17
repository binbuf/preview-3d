#pragma once

#include "model_core/MaterialPayload.h"

#include <cstdint>

struct ufbx_material;

namespace import_worker {

// Converts ufbx's normalized PBR maps (and its FBX fallback maps) to the
// fixed worker/viewer material contract.  `fbxPolicy` adds FBX-only feature
// warnings and honors explicit culling; OBJ keeps its established output.
model_core::MaterialPayload ConvertUfbxMaterial(const ufbx_material& material,
                                                 bool fbxPolicy,
                                                 uint32_t& optionalWarnings);

} // namespace import_worker
