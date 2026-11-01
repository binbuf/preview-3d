# Lessons from F3D: a ranked proposal for format breadth and Studio lighting

**Status:** proposal only, for review. Not an implementation plan of record yet.

**Repositories studied**

- Preview3D `5685101` (2026-10-29), version 0.6.6.
- F3D `7f32e1f` (2026-10-29), version 4.0.0.

**Scope and method**

This is a static study of both codebases plus the existing Preview3D design
docs. I did not run a formal compatibility corpus against the built viewer, so
the failure examples below are derived from the actual dispatch, validation and
adapter code paths, not from a measured open-rate table. The first
recommendation therefore includes building that corpus; it should become the
scoreboard for every later change.

The two goals are treated separately and then combined in the ranking:

1. **Open more valid files.** Reduce false rejection, add the missing high-value
   families, and make "this file is valid but unusual" degrade instead of fail.
2. **Make the default Studio view materially better.** Improve the environment,
   tone mapping, ground contact and material response so PBR assets read
   correctly without user tuning.

---

## Executive summary

F3D's advantage is **not** a better renderer. It is a combination of three
product decisions:

1. **A declarative reader catalog.** Every format is a `reader` object that
   declares its extensions, MIME types, content check, priority score, stream
   support, options, geometry/scene reader and thumbnail eligibility. A factory
   picks the best-scoring reader whose content check passes; the user can force
   a reader or skip the content check.
2. **A permissive compatibility posture.** If a reader can extract useful
   geometry, F3D shows it. Unsupported optional features are warnings or
   approximations, not document failures. It has many readers, and several
   formats have more than one candidate.
3. **Per-format presentation defaults.** F3D ships JSON configuration blocks
   matched by filename glob that set up axis, camera direction, shadows,
   blending, point-sprite mode, background and grid. The same file type looks
   right on first open without user action.

Preview3D's advantage is the opposite side of the same coin: a hard security
boundary, bounded parsing, explicit budgets, a normalized scene contract and a
purpose-built D3D12 renderer. We should **not** trade those away for F3D's
breadth. The opportunity is to keep the security model while adopting F3D's
catalog, content awareness, graceful degradation and per-format defaults.

The biggest self-inflicted open-rate losses in Preview3D today are:

- dispatch is **extension-only**; a valid file with a wrong or missing suffix
  is rejected before any parser sees it;
- there is **one importer per family and no general fallback candidate** (USD
  has a narrow fast-path/OpenUSD handoff, but it only retries for composition,
  not for the other features the host could handle); one unsupported required
  feature rejects the whole file;
- several validators treat **"unknown" as "fatal"** (STEP external documents and
  units, USD procedural primitives and creases, glTF required material
  extensions, 3MF per-corner material variation);
- several common **texture containers** are not decoded (TGA, DDS, HDR, EXR);
- several common **geometry topologies** are dropped (glTF triangle strips/fans,
  lines and points; USD points/curves; STEP wireframe);
- OBJ is reported as having no up axis, so the viewer assumes Z and shows the
  common Y-up OBJ asset lying down.

The biggest Studio gaps are:

- the analytic environment and key light are evaluated in **view space**, so
  reflections and highlights rotate with the camera instead of staying attached
  to the model;
- there is **no prefiltered specular environment**, so metals read as grey
  plastic rather than metal;
- `DisplayMap` is a homebrew luminance roll-off applied **per object before
  alpha blending**; there is no HDR intermediate, exposure control or neutral
  PBR tone map;
- there is **no ambient-occlusion use of the metallic-roughness map's R
  channel**, no ground contact shadow and no SSAO;
- the clear color is a fixed near-black, with no studio backdrop or ground
  plane to ground the object.

### Ranked recommendations at a glance

| Rank | Recommendation | Primary goal | Impact | Effort | Risk |
| ---: | --- | --- | --- | --- | --- |
| 1 | Format catalog, content sniffing and **Open As** | Open rate, maintainability | Very high | M | Low |
| 2 | Best-effort compatibility policy and structured warnings | Open rate | Very high | M | Medium |
| 3 | OCCT host breadth and STEP tolerance | Open rate (CAD) | High | M–L | Medium |
| 4 | Bounded **Assimp** compatibility importer | Open rate (long tail) | High | M–L | Medium–high |
| 5 | USD procedural primitives, points and crease tolerance | Open rate (USD) | High | M | Low–medium |
| 6 | HDR Studio pipeline: IBL, neutral tone map, exposure, light rig | Studio quality | Very high | L | Medium |
| 7 | Texture codec and PBR map expansion | Studio quality / open look | High | M | Medium |
| 8 | Per-format presentation defaults and orientation | Visual correctness | High | S–M | Low |
| 9 | Ground contact shadow and ambient occlusion | Studio quality | High | M–L | Medium |
| 10 | Geometry topology and material model expansion | Long-tail open rate, fidelity | Medium–high | M–L | Medium |

If the team wants the highest-leverage set, do **1, 2, 3, 5, 6, 7 and 8**. Recommendation 4 is the largest breadth lever per unit of code,
but it also adds the largest new parser surface and deserves its own security
review.

---

## 1. What F3D gets right

### 1.1 The reader catalog is the architecture

F3D's `reader` interface (`library/plugin/reader.h:35-215`) declares:

- the reader name and descriptions;
- supported extensions and MIME types;
- a content check, `canRead(stream)`, separate from the extension;
- a **score** used when several readers can read the same file;
- whether it produces a VTK reader (geometry) or a VTK importer (full scene);
- stream support and reader options.

The factory (`library/src/factory.cxx.in:16-53`) walks the registered readers,
calls each `canRead`, and keeps the highest-scoring reader that reports
`SUPPORTED`. It distinguishes `UNSUPPORTED_EXTENSION` from
`UNSUPPORTED_CONTENT`, and the caller gets actionable guidance:

- `--force-reader` to try a named reader even when the extension does not
  match;
- `--skip-content-check` to trust the extension when a content probe is
  unreliable.

The reader declaration is also the **single source of metadata**. F3D's CMake
macro `f3d_plugin_declare_reader` (`cmake/f3dPlugin.cmake:38-66`) generates the
reader registration, MIME metadata, thumbnailer configuration and macOS bundle
extension list from the same declaration. This is why F3D can add formats
without a scavenger hunt through UI, installer and shell code.

**Lesson for Preview3D:** our equivalent metadata is spread across at least:

- `interactive-viewer/src/app/D3D12ImportBridge.h:28-40` (`SourceFormat`);
- `interactive-viewer/src/app/D3D12ImportBridge.cpp:425-441`
  (`ClassifyByExtension`);
- `interactive-viewer/src/app/D3D12ImportBridge.cpp:268-289`
  (`ToBrokerFormat`);
- `shared/model-core/include/model_core/WireFormat.h:120-140`
  (`SourceFormatId`);
- `interactive-viewer/src/platform/ShellIntegration.cpp:40-43`;
- `interactive-viewer/src/app/ActiveInstance.cpp:323-326`;
- `interactive-viewer/src/app/Preview3D.cpp:2230-2238` (open-dialog filters);
- `packaging/installer/Preview3D.nsi` (registration and ProgIDs);
- `thumbnail-provider/FamilyRouting.h` (provider roster).

Adding one format requires editing most of those, plus worker dispatch and
validator rules. That is the structural reason format breadth has been slow.

### 1.2 Breadth comes from optional libraries at the edges

F3D's native plugin covers glTF/GLB, OBJ, STL, PLY, 3DS, VRML, images, VTK
formats and more. Separate plugins add:

- **Assimp**: FBX, COLLADA (`.dae`), DXF, OFF, DirectX `.x`, 3MF, AMF;
- **OCCT**: STEP, IGES, BREP, XBF, FreeCAD `.FCStd`;
- **OpenUSD**: USD/USDA/USDC/USDZ, including points and procedural primitives;
- **web-ifc**: IFC;
- **Alembic**, **PDAL**, **VDB**, HDF: scientific/VFX families.

Not all of that is relevant to a Windows 3D-asset viewer. The relevant long-tail
gap versus Preview3D is:

| Family | Preview3D | F3D | Suggested action |
| --- | :---: | :---: | --- |
| glTF/GLB, OBJ, FBX, STL, PLY, 3MF, USD, STEP | yes | yes | Keep native adapters; fix the gaps below |
| IGES/IGS | no | yes | Add through the existing OCCT host |
| BREP, XBF, FCStd | no | yes | Add through the existing OCCT host |
| COLLADA `.dae` | no | yes | Add through a bounded Assimp adapter |
| 3DS, DXF, X, OFF, AMF | no | yes | Add through the same Assimp adapter |
| VRML/WRL, X3D | no | VRML yes | Optional; Assimp covers X3D |
| 3D Gaussian splats (`.splat`, `.spz`, 3DGS PLY) | no | yes | Optional, separate renderer work |
| Images as 2D planes (PNG/JPEG/BMP/TGA/HDR/EXR/TIFF) | no | yes | Optional, cheap WIC/decoder path |
| IFC, Alembic, VDB, PDAL, VTK formats | no | yes | Defer unless the audience demands them |

F3D's own format limitations documentation is useful honesty: its Assimp path
can produce empty scenes or artifacts, and DXF support is limited to polylines
and 3D faces (`doc/user/14-LIMITATIONS_AND_TROUBLESHOOTING.md:12-18`). Adding
Assimp is not a guarantee of quality; it is a breadth/coverage tool. Our
sandbox contains the crash risk, but not the visual quality risk.

### 1.3 Per-format defaults are first-class, not afterthoughts

F3D ships configuration blocks such as
`plugins/native/configs/config.d/10_native.json` and
`plugins/assimp/configs/config.d/10_assimp.json`. They match filename globs and
set, for example:

- `up` and `camera-direction` for 3DS/STL/3MF (`+Z`) and DXF (`-Z`);
- `shadows: true` for STL, 3MF, 3DS, FBX, DAE, OFF;
- point-sprite mode, absolute size, black background, direct coloring and
  sorted blending for `.splat`/`.spz`;
- orthographic camera and 2D interaction for images;
- coloring mode and scalar-bar settings for scientific formats.

**Lesson for Preview3D:** a default is not just a renderer setting. It is a
property of the format and often of the material/geometry class. Our viewer has
a single global Studio/Clay/Directional selection and a global grid; it does not
know that an STL/STEP/3MF is a CAD part, that an OBJ is usually Y-up, or that a
PLY may be a point cloud.

### 1.4 F3D's rendering defaults are simple but physically grounded

F3D's default is not an elaborate cinematic pipeline. It is:

- VTK PBR materials, with legacy diffuse/specular materials converted to PBR
  and colors linearized (`vtkext/private/module/vtkF3DMetaImporter.cxx:487-527`);
- a **camera-relative multiple-light kit** when the scene has no authored
  lights (`vtkF3DMetaImporter.cxx:608-643`);
- optional image-based lighting from an embedded default HDRI
  (`resources/defaultHDRI.png`, or `.hdr` on newer VTK;
  `vtkF3DRenderer.cxx:1430-1717`), with a generated specular prefilter and
  spherical-harmonics diffuse cache;
- optional neutral PBR tone mapping (`vtkToneMappingPass::NeutralPBR`,
  `vtkF3DRenderer.cxx:578-584`);
- optional shadow maps, SSAO, FXAA/SSAA/TAA and depth-peeled/stochastic
  transparency (`vtkF3DRenderer.cxx:519-617`).

F3D's defaults disable most effects (`hdri.ambient=false`, `shadows=false`,
`tone_mapping=false`, antialiasing none), but the **PBR + multi-light kit**
baseline is already enough to make most assets read correctly. Our analytic
Studio baseline is comparable in intent but has the view-space/IBL issues noted
above.

### 1.5 F3D preserves authored scene intent when it exists

`vtkF3DAssimpImporter.cxx:67-160` imports cameras and lights from Assimp scenes.
`vtkF3DUSDImporter.cxx` supports USD points, procedural primitives and skinning
(though not USD cameras). F3D can also load multiple files into one scene
(`library/src/scene_impl.cxx:334-432`).

**Lesson for Preview3D:** we do not need to make authored lights the default,
but we should not discard them. A "Scene" presentation preset that uses authored
cameras/lights, with Studio as the default, is a low-risk fidelity win. It is
lower priority than the catalog/studio work, but it should be designed into the
normalized scene instead of being impossible to add later.

### 1.6 What not to copy

- **Do not adopt VTK as the renderer or as a general dependency.** It would
  replace the D3D12 architecture, add a large runtime and plugin surface, and
  undo the packaging/isolation work. Use F3D's *ideas*, not its stack.
- **Do not load parsers into the viewer process.** F3D's plug-in model is
  explicitly designed around in-process VTK readers. Our AppContainer boundary
  is a differentiator and must remain.
- **Do not copy F3D's Windows thumbnail provider.** It launches `f3d.exe`,
  writes a temp PNG and decodes it (`winshellext/F3DThumbnailProvider.cxx`).
  Our COM provider is faster and stronger; only its format coverage is
  instructive.
- **Do not adopt unrestricted path resolution or dynamic plug-in loading.**
  F3D's reader may open paths relative to the file. Our broker/canonical-
  containment model is deliberate and should stay.
- **Do not chase every scientific format.** VTK XML, DICOM, NRRD, VDB, PDAL,
  Alembic and IFC are real F3D strengths but poor fits for the current product
  scope unless the target audience changes.

---

## 2. Why valid files fail in Preview3D today

This section is deliberately concrete: each row points at the code path that
turns a valid or common file into a failure. These are the cases the
recommendations below are designed to remove.

| Symptom | Code path | Why it happens | User-visible result |
| --- | --- | --- | --- |
| A valid glTF/GLB named `.bin`, `.txt`, or with no suffix never opens | `ClassifyByExtension` at `D3D12ImportBridge.cpp:425-441`; `Preview3D.cpp:2093-2100` | Dispatch is extension-only; no content sniff and no Open As | "Open a .glb, .gltf..." error, even though the bytes are a supported model |
| A modern glTF requiring a visual-only material extension fails | `GltfAdapter.cpp:400-411`, `111-118`; parser mask at `2399-2412` | fastgltf returns `UnknownRequiredExtension`/`MissingExtensions`; we treat all as `UnsupportedRequiredFeature` and fail | "This model requires an unsupported feature" |
| A glTF with `TRIANGLE_STRIP`, `TRIANGLE_FAN`, lines or points renders empty or missing parts | `GltfAdapter.cpp:1210-1212` | Every non-`Triangles` primitive is silently skipped | Empty geometry or incomplete model |
| A `3MF` with per-corner texture/sampler variation fails | `ThreeMfAdapter.cpp:382-390` | Different texture/sampler per triangle corner is treated as a required unsupported feature | Whole model fails |
| A STEP assembly with `FILE_POPULATION`/`DOCUMENT_FILE` fails before OCCT sees it | `StepPart21Preflight.cpp:423-430` | A case-insensitive substring search over the whole text rejects any occurrence, including comments/strings; zero external documents allowed | Valid self-contained geometry is rejected |
| A STEP file without an authored length unit fails | `StepXdeAdapter.cpp:1480-1503` | `FileUnits` must be non-empty and a unit must be resolvable; no millimetre default | Valid CAD file rejected |
| A STEP file whose geometry needs repair, or whose per-definition face/edge/triangle count exceeds the budget, fails | `StepTessellationProfile.h:144-165` (`StepHealingPolicy::None`); `StepXdeAdapter.cpp:1164-1177, 1259-1260` | No repair/healing path, by deliberate policy; per-definition budgets are hard failures | "Could not be tessellated" |
| A USD scene made of `Cube`, `Sphere`, `Cylinder`, `Cone`, `Capsule` or `Points` fails | `UsdAdapter.cpp:394-400`; `OpenUsdHost.cpp:1485-1506` | TinyUSDZ marks visible procedural primitives as unsupported; the OpenUSD host only emits `UsdGeomMesh`; the broker only retries on `UnsupportedComposition` (`ImportSession.cpp:1474-1488`) | "Requires an unsupported feature" or empty geometry |
| A USD mesh with creases or non-`none` subdivision fails | `UsdAdapter.cpp:367-372`; `OpenUsdHost.cpp:1164-1181` | Creases are treated as required-unsupported, not approximated as a control cage | Valid subdiv asset rejected |
| An OBJ opens but lies on its side | `ObjAdapter.cpp:561` sets `UpAxisId::Unknown`; `SharedSectionValidator.cpp:230-236` requires Unknown for OBJ; `GroundAxis.h:21-26` maps Unknown to Z | Most OBJ assets are authored Y-up, but the viewer assumes Z | Wrong orientation on a common format |
| An FBX with TGA/DDS textures opens with fallback materials | `ImageFormatSniff.cpp:63-93`; `PixelFormats.h:29-36`; texture policy in `03-file-formats-and-ingestion.md:249-260` | TGA, DDS, HDR and EXR are not decoded | "Some textures could not be loaded"; white/grey asset |
| An FBX/OBJ with only NURBS, lines or points fails with empty geometry | `FbxAdapter.cpp` unsupported geometry checks; `ObjAdapter.cpp:507-512` only emits triangles | No line/point topology in the wire format (`WireFormat.h:40-43`) and adapters only emit triangles | "No displayable geometry" |
| A large Tier B file fails with a resource limit | `03-file-formats-and-ingestion.md` Tier B ceilings (2 GiB / 20M triangles) | Limits are hard failures, not a reduced-detail fallback | "Too large to preview" |
| A missing sidecar image is found, but a missing buffer or MTL is not | `03-file-formats-and-ingestion.md:24-80`, `SidecarPathResolver*` | The package lookup is image-only; buffers/MTL/USD layers keep strict rejection | Model fails or loses materials; user can point at a folder for images only |
| A file with an unsupported optional feature shows a generic warning | `WireFormat.h:111-118`; `RenderThread.cpp:61-73`; `Preview3D.cpp:2015-2017` | Only a count is carried; the host cannot say which feature was ignored | The user cannot tell whether the preview is faithful |

The pattern is consistent: **we detect a condition, then choose the most
conservative outcome even when a useful preview is still possible.** That is the
right default for security, but the wrong default for optional visual features
and for formats whose "required" markers are often optimistic.

---

## 3. Ranked recommendations

### 3.1 Recommendation 1 — Format catalog, content sniffing and "Open As"

**Goal:** one source of truth for what a format is, how to recognize it, which
importer serves it, and what its defaults are.

**What F3D does:** `reader` declarations plus a scored factory
(`library/plugin/reader.h`, `library/src/factory.cxx.in`). Content checks and
extension checks are separate; a forced reader or skipped content check is
available when detection is wrong.

**Proposal**

1. Add `shared/model-core/include/model_core/FormatCatalog.h` (or a small
   `shared/format-catalog` header) with a compile-time table of descriptors:

   ```cpp
   struct FormatDescriptor {
       FormatId id;                       // stable, append-only
       std::wstring_view stableName;      // "gltf", "dae", "iges", ...
       std::wstring_view displayNameKey;  // localization key
       std::span<const wchar_t* const> extensions;
       std::span<const MagicSignature> signatures;
       ImporterKind importer;             // native worker, USD host, CAD host, Assimp
       uint8_t priority;                  // tie-breaker when several match
       UpAxisId defaultUpAxis;
       UnitPolicy unitPolicy;
       PresentationProfile presentation;  // recommendation 8
       ThumbnailFamily thumbnail;         // none until a provider adapter exists
       CapabilityFlags capabilities;      // sidecars, archive, coarse proxy, ...
   };
   ```

2. Make every consumer read the table instead of maintaining its own list:
   `ClassifyByExtension`, `ToBrokerFormat`, `SourceFormatLabel`,
   `ShellIntegration`, `ActiveInstance`, the open-dialog filter, the installer
   registration list, the thumbnail roster and the docs matrix. A test should
   assert that the installer and UI lists equal the catalog (no drift).

3. Add a bounded primary-file sniffer in the trusted process (a few dozen KiB
   from the head, plus a bounded ZIP central-directory probe for archive-based
   formats; no structural model parsing). At minimum:
   - glTF: JSON containing `"asset"` and `"version"`; GLB `glTF` magic;
   - STL: binary size/prefix or ASCII `solid`;
   - PLY: `ply`;
   - OBJ: `v`/`f`/`mtllib` tokens (tolerant);
   - FBX: `Kaydara FBX Binary  \0` or ASCII `; FBX`;
   - 3MF/USDZ: ZIP magic plus a bounded central-directory check for
     `[Content_Types].xml` / `*.usdc`;
   - USD: `PXR-USDC` magic or `#usda`;
   - STEP: `ISO-10303-21`;
   - IGES: section-letter layout; DAE `<COLLADA`; 3DS `0x4D4D`; AMF `<amf`;
   - OFF/PLY/text formats by header.
   Do not trust the sniff alone for security: it only chooses a candidate.

4. Route by candidates, not a single extension switch. If the extension is
   known and content matches, use it. If they disagree, try the content match
   and offer the extension match as an alternative. If the extension is
   unknown, use the content match. Never parse beyond the sniff in the trusted
   process.

5. Add **Open As**:
   - UI: an "Open as..." item in the open/failure flow, listing formats from
     the catalog;
   - CLI: `--format <stableName>` and `--skip-content-check`;
   - diagnostics: "Detected as X; opened as Y" in the info panel.

6. Add `--list-formats` (mirroring F3D's `--list-readers`) and a generated
   "Supported formats" table for the README/docs.

**Why rank 1:** it is a prerequisite for every later format addition, it fixes
the "valid bytes, wrong suffix" class immediately, and it removes the duplicate
lists that make breadth expensive. It adds no parser code and therefore no new
attack surface.

**First slice (small):** table + `ClassifyByExtension` + sniffer + CLI
`--format` + tests. UI Open As can follow.

**Full slice:** installer generation, docs generation, thumbnail roster
validation, content-vs-extension mismatch handling.

**Acceptance**

- A GLB renamed `.bin` opens as glTF through sniffing or Open As.
- A valid `.gltf` with a `.txt` suffix opens with a warning that the extension
  was unexpected.
- A malformed file with a correct extension still fails with the existing typed
  error.
- Every catalog extension appears exactly once in the installer and UI.
- `--list-formats` output matches the installed associations.

**Effort:** M (roughly 2–3 engineer-weeks for the full surface).
**Risk:** low. No new parser; the main risk is touching many places at once, so
land it behind tests that compare generated lists.

---

### 3.2 Recommendation 2 — Best-effort compatibility policy and structured warnings

**Goal:** stop rejecting a valid file because an *optional visual* feature is
unknown, while still failing closed for a feature that prevents trustworthy
geometry.

**What F3D does:** reader-specific `canRead` and parsing; unsupported features
generally produce warnings or fallback behavior. Its default is "show what you
can."

**Proposal**

1. Define a small severity model in shared code:

   ```text
   Critical   -> cannot produce trustworthy geometry; fail with feature ID
   Degraded   -> produce a documented approximation; warn with feature ID
   Ignored    -> no visible effect; debug-log only
   ```

2. Carry a bounded **CompatibilityReport** to the host: a fixed-size list of
   `{featureId, severity, count}` records, not strings. The host owns the
   localized text. This replaces the current single
   `optionalFeatureWarnings` count and lets the info panel say exactly what was
   approximated.

3. Apply it per format:

   **glTF.** Enable every material extension fastgltf supports and whose data
   path we can consume in the parser mask, not just the ten currently enabled
   (`GltfAdapter.cpp:2399-2412`) — for example clearcoat/sheen/specular parse
   successfully even before the renderer can draw the lobes. Then classify
   required extensions:

   | Extension class | Examples | Action |
   | --- | --- | --- |
   | Geometry/decoder-critical | `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `KHR_mesh_quantization` | Decode; fail if undecodable |
   | Texture-critical | `KHR_texture_basisu`, `EXT_texture_webp` | Decode; if a required texture cannot be decoded, use the documented fallback |
   | Visual material | `KHR_materials_clearcoat`, `sheen`, `specular`, `iridescence`, `anisotropy`, `emissive_strength`, `ior`, `volume`, `dispersion` | Parse if possible, approximate or ignore, warn |
   | Scene/metadata | `KHR_lights_punctual`, `KHR_materials_variants`, `KHR_xmp_json_ld` | Ignore for default Studio; select the default variant |
   | Instance-critical | `EXT_mesh_gpu_instancing` | Expand or show one instance with a warning |
   | Truly unknown required | anything else | Keep core geometry if present and warn; fail only if no usable geometry remains |

   To make a truly unknown required extension parseable, pre-scan
   `extensionsRequired` with simdjson, classify it, and filter non-critical
   names from the JSON before fastgltf sees it (or retry once with a filtered
   copy). Never filter an extension that replaces geometry or decoding.

   **3MF.** Keep the lib3mf official parser, but:
   - ignore unknown optional namespaces/extensions unless the model's
     `requiredextensions` says the visible geometry depends on them;
   - split a triangle when its corners use different textures/samplers instead
     of failing the model (`ThreeMfAdapter.cpp:382-390`);
   - if a required extension is unsupported, try the authored fallback
     representation; fail only when no visible geometry can be recovered.

   **USD.** Change the fast-path classification so features the OpenUSD host
   can handle are retryable rather than terminal: procedural primitives,
   `UsdGeomPoints`, creases/subdivision and other non-TinyUSDZ features should
   return a `NeedsCompatibilityHost`/`UnsupportedComposition` result (see
   recommendation 5). Do not retry malformed, unsafe or limit failures.

   **STEP.** Replace the raw substring check with entity-aware header parsing;
   treat external documents and absent units as warnings, not fatal errors (see
   recommendation 3).

   **All formats.** When a resource ceiling is reached and a coarse proxy is
   possible, publish a reduced-detail preview with a warning instead of failing.
   The existing `ImportStatusPayload` flags can carry "reduced detail"; the
   compatibility report can name the specific limit.

4. Improve the user-facing warning text. Today
   `warning.optionalFeatures` is a generic "Some optional glTF features are not
   supported" (`RenderThread.cpp:61-73`), and STEP gets a generic
   "Some STEP surfaces..." (`Preview3D.cpp:2015-2017`). Use the feature IDs to
   say, for example: "Clearcoat is not rendered; base PBR is shown" or
   "External STEP references were skipped; the main document is shown."

**Why rank 2:** this is the highest-leverage change for files that already use a
supported parser. It removes a large class of `UnsupportedRequiredFeature`
failures without adding a new dependency.

**First slice:** glTF extension mask + severity table + `CompatibilityReport`
for glTF; relax USD creases to a warning.

**Full slice:** 3MF, USD and STEP policies; over-limit proxy fallback.

**Acceptance**

- A glTF requiring `KHR_materials_clearcoat` or `KHR_materials_specular` opens
  with base PBR and a specific warning.
- A glTF requiring Draco with an undecodable primitive still fails typed.
- A 3MF with per-corner material variation opens with split triangles.
- A malformed 3MF/STEP/USD still fails and does not get a more permissive retry.
- No path, network, budget or containment rule is relaxed.
- Hostile-worker and fuzz suites remain green.

**Effort:** M.
**Risk:** medium. The hard part is drawing the critical/visual line and proving
that a degraded path cannot hide missing geometry. Use a feature table and
fixtures; do not scatter ad-hoc `if` checks.

---

### 3.3 Recommendation 3 — OCCT host breadth and STEP tolerance

**Goal:** make the existing OCCT host serve the rest of the CAD family, and stop
rejecting valid STEP files for header/unit reasons.

**What F3D does:** one OCCT plugin reads STEP, IGES, BREP, XBF and FCStd with
the same reader options (`plugins/occt/CMakeLists.txt:57-133`). It does not
pre-reject external documents or missing units; OCCT reads what it can.

**Proposal**

1. Generalize `Preview3DStepHost.exe` into a `Preview3DCadHost` (name can stay
   for packaging) with a `CadFormat` field: `Step`, `Iges`, `Brep`, `Xbf`,
   `FCStd`. Keep the same AppContainer, handle-only input, Job Object, shared
   section and validator.

2. Add bounded preflights/adapters:
   - **IGES**: section-letter/record preflight; size/entity limits; bounded
     `IGESControl_Reader` or `IGESCAFControl_Reader`.
   - **BREP**: `BRepTools::Read` from a mapped stream; header/size checks.
   - **XBF**: binary magic/version check; `BinTools`/`BinXCAFDrivers`.
   - **FCStd**: reuse the existing bounded ZIP preflight; parse the document
     XML with limits; read the `.brp` shape and colors. Start with the common
     single-part/PartDesign case; treat unsupported FreeCAD objects as
     warnings.
   All of these must use the same normalized `SceneMetadata`/chunk writer, with
   up axis `Unknown` and a verified metre factor where available.

3. STEP tolerance changes:
   - **External documents:** parse only header/meta entities outside comments
     and strings; if `FILE_POPULATION`/`DOCUMENT_FILE` is present, attempt the
     main document and treat unresolved externals as a warning. Fail only if
     the main document has no transferable roots.
   - **Units:** if no length unit is authored, assume millimetres and warn;
     if multiple/contradictory units are authored, choose the first/dominant
     unit, convert what can be converted, and warn. Never silently claim an
     exact unit when none was authored.
   - **Healing:** add an opt-in, bounded repair pass (`ShapeFix`/`XSAlgo`) only
     when read or tessellation fails, with a warning and a profile-version bump.
     It must be deterministic and must not run by default on already-valid
     geometry.
   - **Wireframe:** enable `read_wire` and emit line geometry when a shape has
     only wires/edges (depends on recommendation 10); until then, warn and skip.
   - **Per-definition failure isolation:** keep the current behavior of
     skipping an unmeshable definition with a warning; do not fail the whole
     assembly for one bad part.
   - **Import quality:** expose the existing tessellation profile as a
     Draft/Balanced/Full setting (and, later, per-format reader options such as
     STEP/IGES deflection and `read_wire`, mirroring F3D's reader options).
     The default stays Balanced; the setting is part of the import/cache
     identity.

4. Update the scope and decision records:
   - `docs/design/01-product-scope.md` FR-01 and explicit exclusions currently
     say IGES/IFC/AMF/DAE are out of scope;
   - ADR-017 says STEP is self-contained only.
   These need explicit amendments if the team accepts this recommendation.

**Why rank 3:** IGES and BREP are common CAD interchange formats, and STEP
external/unit rejection is a self-inflicted failure that affects real CAD
exports. The OCCT dependency and isolation already exist, so this is mostly
adapter/preflight work rather than a new runtime.

**First slice:** STEP external-document and unit tolerance; IGES + BREP.

**Full slice:** XBF + FCStd; optional repair; wireframe.

**Acceptance**

- `cheese.stp`, `cheese.igs`, `cheese.brep`, `cheese.xbf` and a representative
  FreeCAD `.FCStd` open through the isolated host.
- A STEP file with external document declarations but valid main geometry opens
  with a warning.
- A STEP file with no unit opens with a millimetre warning; a file with two
  contradictory units opens with a warning and a documented choice.
- Malformed/oversized CAD files still fail typed; no path or network access.
- The STEP qualification fixtures and hostile-worker suite remain green.

**Effort:** M–L (2–3 engineer-weeks, mostly OCCT adapter/preflight work).
**Risk:** medium. OCCT's readers are large; keep the same job/commit/time
budgets and preflight every new container. Do not link OCCT anywhere new.

---

### 3.4 Recommendation 4 — A bounded Assimp compatibility importer

**Goal:** cover the common mesh long tail without hand-writing a parser per
format.

**What F3D does:** the Assimp plugin declares FBX, COLLADA, DXF, OFF, DirectX
`.x`, 3MF and AMF (`plugins/assimp/CMakeLists.txt:42-118`). It uses a custom
`CanReadFile` that inspects magic and XML roots
(`vtkF3DAssimpImporter.cxx:1382-1449`) and a score to resolve overlapping
readers.

**Proposal**

1. Add Assimp as a **worker-only** compatibility adapter, not as the universal
   importer and not in the trusted viewer. It should cover the formats we do not
   already have:
   - `.dae` (COLLADA), `.3ds`, `.dxf`, `.x`, `.off`, `.amf`, `.x3d`;
   - optionally `.3mf`/`.fbx` as a last-resort fallback only when the native
     adapter returns a typed `UnsupportedRequiredFeature`, never after a
     successful batch or on malformed/unsafe/empty input.
2. Implement a custom `Assimp::IOSystem` backed by the existing broker sidecar
   client. Absolute paths, `..`, UNC/device paths, ADS, URLs and network
   schemes are denied by the same policy as the other adapters. Embedded
   textures are accepted within the existing decoded-byte budgets.
3. Set hard limits before calling Assimp: source bytes, node/mesh/material/
   texture counts, post-process flags (triangulate, generate normals, join
   identical vertices, no animation/caches), allocator/progress callbacks and
   cancellation checks between bounded units.
4. Normalize Assimp output into the existing `BoundedChunkWriter` and
   `MaterialPayload` contract. Decode textures through the existing texture
   policy (recommendation 7 adds TGA/DDS/HDR/EXR). Do not let Assimp write files
   or load plug-ins.
5. Add a per-format fuzz corpus and a job-commit budget. Assimp has a large
   historical CVE surface; the AppContainer boundary is necessary but not
   sufficient. Treat this as a new parser gate.

**Why rank 4:** one dependency unlocks six common formats and provides a
fallback for two more. It is the largest breadth-per-code change after the
catalog, but it must be scoped carefully.

**Tradeoff:** ADR-006 rejected "Assimp as a universal runtime importer." This
proposal does not reverse that. It adds Assimp as a **bounded compatibility
importer for formats with no native adapter**, in the isolated worker. The ADR
should be amended explicitly, including the fuzz/budget/packaging obligations.

**First slice:** DAE + 3DS + OFF + AMF, read-only, no external textures.

**Full slice:** DXF/X/X3D; external texture brokering; FBX/3MF fallback.

**Acceptance**

- Representative `duck.dae`, `iflamigm.3ds`, `teapot.off`,
  `cube-with-hole.amf` and a small DXF open.
- A malformed/oversized Assimp input fails typed and does not crash or hang.
- No path escape, no network, no child process, no file write.
- New parser fuzz target and SBOM/license entries are in place.
- Existing native adapters are unchanged when their formats are opened.

**Effort:** M–L (2–3 engineer-weeks plus fuzzing).
**Risk:** medium–high: new parser surface, package size, visual variability.
Do not use Assimp for formats where our native adapter is better (glTF, OBJ,
FBX by default, STL, PLY, USD, STEP).

---

### 3.5 Recommendation 5 — USD procedural primitives, points and crease tolerance

**Goal:** open the large class of USD scenes that are not triangle meshes, and
make the fast-path/host handoff cover them.

**What F3D does:** its OpenUSD importer explicitly tessellates
`UsdGeomSphere`, `Cube`, `Capsule`, `Cylinder`, `Cone` and `Points`
(`plugins/usd/module/vtkF3DUSDImporter.cxx:764-953`), including axis handling
and per-point colors. It also supports subdivision levels as a reader option.

**Current Preview3D behavior:** the TinyUSDZ fast path marks visible procedural
primitives and curves as `unsupportedRequired` (`UsdAdapter.cpp:394-400`); the
OpenUSD host only emits `UsdGeomMesh` and fails with `EmptyGeometry` if none are
found (`OpenUsdHost.cpp:1485-1506`); and the broker only retries the host for
`UnsupportedComposition` (`ImportSession.cpp:1474-1488`). A simple
`def Cube` scene therefore cannot open.

**Proposal**

1. Add a bounded `Gprim` tessellator to the OpenUSD host for sphere, cube,
   cylinder, cone and capsule. Use fixed low-segment counts (e.g. 24×16 sphere,
   24-segment cylinders/cones), honor `radius`, `height`, `size`, `axis` and
   `extent` where authored, apply the local transform, and generate normals.
2. Add `UsdGeomPoints` as a `PointList` chunk with display color/opacity.
3. Treat creases/subdivision as a **control-cage approximation** with a
   warning, not a fatal error. If a subdivision level option is added later,
   subdivide up to a bounded level; otherwise render the cage.
4. Change the fast-path result for host-supported features from
   `UnsupportedRequiredFeature` to a retryable classification
   (`UnsupportedComposition` or a new `NeedsCompatibilityHost`). The broker must
   still require that no fast-path batch was accepted before the retry, so the
   two producers can never mix.
5. Keep curves/NURBS as a warning until recommendation 10 adds line topology;
   then emit polylines or a tube approximation.

**Why rank 5:** simple USD assets are often authored with procedural primitives
because they are compact and DCC-friendly. Our current behavior fails them in
both paths. This is a contained change to a host we already ship.

**First slice:** sphere/cube/cylinder/cone/capsule + points in the OpenUSD host;
retry classification in the fast path.

**Full slice:** crease/subdivision control-cage warnings; curves.

**Acceptance**

- F3D's `primitives.usda`, `primitivesZ.usda` and `usd_points_rgb.usda` open
  through the host.
- `subdiv_cube_cc.usda` opens as a control cage with a warning.
- A mesh-only USD file still takes the fast path and produces identical output.
- A malformed/unsafe USD stage still fails and is not retried.
- Host commit/time limits and hostile-worker tests remain green.

**Effort:** M (roughly 1–2 engineer-weeks).
**Risk:** low–medium. Tessellation is deterministic and product-owned; the main
risk is the fast-path/host fallback rule, which deserves a protocol test.

---

### 3.6 Recommendation 6 — HDR Studio pipeline: IBL, neutral tone mapping, exposure and light rig

**Goal:** make the default Studio view physically plausible for metals, glass,
white/black materials and rough dielectrics, without user tuning.

**What F3D does:** PBR materials, a multi-light kit, optional HDRI-based IBL
with a prefiltered specular cache and SH diffuse cache, optional neutral PBR
tone mapping, and optional shadows. Its default HDRI is a studio softbox
environment (`resources/defaultHDRI.png`, or `.hdr` on newer VTK), even though
ambient IBL is off by default.

**Current Preview3D behavior:**

- `StudioEnvironment` and `EnvironmentIrradiance` are analytic gradients with
  one softbox (`D3D12ViewerPath.cpp:274-304`);
- the vertex shader transforms positions/normals into **camera space**
  (`VSMain`, lines 202-217), and the pixel shader evaluates the environment and
  key in that space (lines 349-393), so reflections and highlights are
  camera-locked;
- `DisplayMap` is a per-object luminance roll-off followed by gamma
  (`D3D12ViewerPath.cpp:294-304`), applied before alpha blending;
- there is no exposure control, HDR target, environment intensity or
  background mode;
- the metallic-roughness map's R channel (ambient occlusion) is unused
  (`D3D12ViewerPath.cpp:357`).

**Proposal**

1. **Add a real studio environment.** Two viable sources:
   - a small embedded HDRI (a 512×256 or 1K studio softbox asset, as F3D does);
     or
   - a procedural studio environment generated at startup from a few
     area-light rectangles and a gradient, then baked into a cubemap.
   The procedural route avoids licensing and asset delivery; the embedded HDRI
   route gives more natural reflections. A hybrid (procedural default, optional
   user HDRI later) is reasonable.

2. Precompute:
   - diffuse irradiance (SH9 or a small diffuse cubemap);
   - a GGX-prefiltered specular cubemap with a bounded mip chain;
   - an optional background cubemap or a screen-space studio gradient.
   Cache the result for the session; do not re-filter per frame.

3. **Move to an HDR scene target.** Render opaque/transparent geometry into an
   `R16G16B16A16_FLOAT` target plus depth, then run a full-screen tone-map +
   sRGB pass. This fixes:
   - alpha blending in tone-mapped space;
   - specular clipping and desaturation;
   - emissive/transmission composition;
   - a future bloom/FXAA/SSAO pass.
   UI/overlay stays after the 3D pass as it is today.

4. **Use a neutral PBR tone mapper** as the Studio default (Khronos PBR Neutral
   or an equivalent neutral curve), with an exposure control and an optional
   ACES/Filmic/None choice. The current homebrew curve should not be the only
   option.

5. **Separate the light rig from the environment.** Add a key/fill/rim rig with
   independent intensities. Keep it camera-relative for predictable object
   readability, but make the environment world-fixed (or yaw-fixed) so
   reflections move across the surface as the camera orbits. Add a single
   `Studio intensity` control that scales direct and IBL together, as F3D does
   with `render.light.intensity` and `IBLIntensity`.

6. **Material-aware presets.** Use imported material/geometry statistics to
   select a small, stable set:
   - **Studio PBR** (default for textured glTF/FBX/USD): IBL + soft key/fill,
     neutral tone map, moderate exposure.
   - **Studio CAD** (untextured STL/STEP/3MF/IGES): stronger key/fill, low
     specular, optional ground shadow/AO, neutral grey material.
   - **Studio Scan/Sculpt** (high-poly PLY/FBX): soft wrap lighting, low
     specular, AO.
   - **Studio Transparent**: brighter backdrop/checkerboard, IBL, exposure
     compensation.
   - **Point cloud**: unlit colored splats, no IBL, dark neutral background.
   Auto-select by format and material summary; keep a manual override.

7. **Background:** add a neutral studio backdrop option (flat mid-grey or a
   subtle vertical gradient), keeping the current dark UI-matched background as
   an option. Material judgment is easier against a neutral mid-grey than
   near-black.

**Why rank 6:** this is the single largest visual-quality change and directly
addresses the default Studio goal. It is lower than the open-rate items only
because it does not add file coverage.

**First slice:** procedural studio cubemap + prefiltered specular + neutral
tone map in the existing shader, no HDR target yet.

**Full slice:** HDR target + post pass, exposure, background modes,
material-aware presets.

**Acceptance**

- A material-sphere test grid (white, black, red dielectric, rough metal,
  polished metal, glass) reads correctly under Studio without manual changes.
- Metals show distinct softbox highlights; reflections move when the camera
  orbits.
- White materials do not clip to flat white; dark materials retain form.
- The background can be dark, neutral grey or transparent.
- Frame time and memory stay within the existing performance envelope.
- Screenshot goldens cover Studio, Clay, Directional and a transparent asset.

**Effort:** L (roughly 3–4 engineer-weeks for the full HDR/IBL slice).
**Risk:** medium. Visual changes are subjective and easy to regress; build the
goldens first, and keep the old analytic path until the new one is proven.

---

### 3.7 Recommendation 7 — Texture codec and PBR map expansion

**Goal:** stop losing the materials that make a model look like the authored
asset.

**What F3D does:** its native image readers cover TGA, HDR and EXR (plus
PNG/JPEG/BMP/WebP/TIFF), and its PBR pipeline consumes base color, ORM,
emissive and normal maps. It ships an EXR reader and an HDR default environment.

**Current Preview3D behavior:**

- `SniffImageFormat` and `HasDecodableImageExtension` accept only
  PNG/JPEG/BMP/TIFF/WebP/KTX2 (`ImageFormatSniff.cpp:63-93`);
- `PixelFormatId` has RGBA8, BC7, BC5, BC3, BC1 (`PixelFormats.h:29-36`);
- TGA, DDS, HDR and EXR references are skipped or fall back;
- the metallic-roughness R channel is not used for AO;
- `emissiveFactor` has no `KHR_materials_emissive_strength` multiplier.

**Proposal**

1. Add decoders in the import worker (never the viewer):
   - **TGA**: small bounded decoder or `stb_image` with strict size/format
     checks; verify header/palette/RLE bounds before decode.
   - **DDS**: either WIC's inbox DDS decoder or a small DDS/BCn loader that
     passes BC1/BC3/BC5/BC7 blocks through to the existing GPU formats. This is
     the highest-value addition for downloaded FBX/OBJ packages.
   - **HDR (Radiance RGBE)**: needed for environment maps and emissive/HDR
     textures; decode to linear float or tonemap to RGBA8 for non-environment
     slots.
   - **EXR**: use a small bounded decoder (e.g. tinyexr) for normal/roughness/
     emissive maps; linear data, no sRGB transform.
2. Extend `PixelFormatId` with the formats actually needed for direct upload
   (BC6H for HDR, RGBA16F if float textures are sampled directly). If the
   target GPU path cannot sample a format, decode/tonemap on CPU instead of
   failing the model.
3. Add the new extensions to the sidecar allowlist and the package-name search
   (`03-file-formats-and-ingestion.md:24-80` and the broker's per-format
   extension set).
4. Use the metallic-roughness map's R channel as ambient occlusion (glTF packs
   ORM; FBX/USD often do too). Add an `occlusionStrength`/factor if the source
   format provides one.
5. Bake `KHR_materials_emissive_strength` into the emissive factor and add a
   clearcoat/sheen/specular material extension later (recommendation 10).
6. Keep the existing decoded-texture budget and downscale policy; a missing or
   over-budget optional texture should still fall back rather than fail.

**Why rank 7:** it converts "opens but looks wrong" into "opens correctly." TGA
and DDS alone cover a large share of older FBX/OBJ downloads; EXR/HDR cover
modern DCC exports and environment work.

**First slice:** TGA + DDS + AO map.

**Full slice:** HDR/EXR + float/BC6H + texture-sidecar policy.

**Acceptance**

- An FBX with TGA and DDS textures renders its authored maps.
- An EXR normal/roughness map is used or skipped with a specific warning, not a
  fatal error.
- A malformed TGA/DDS/EXR fails typed and cannot overrun its budget.
- The decoded-texture budget and downscale behavior still hold.
- New decoders are fuzzed and listed in the SBOM/license notices.

**Effort:** M (roughly 2 engineer-weeks).
**Risk:** medium. New decoders are parser surface; keep them in the worker and
fuzz them. Do not re-enable a general DirectXTex path without a budget review.

---

### 3.8 Recommendation 8 — Per-format presentation defaults and orientation

**Goal:** make a file look right on first open without user setup.

**What F3D does:** configuration blocks by glob set up axis, camera direction,
shadows, grid, point sprites, blending, background and coloring per format
(`plugins/native/configs/config.d/10_native.json`,
`plugins/assimp/configs/config.d/10_assimp.json`).

**Current Preview3D behavior:** one global Studio/Clay/Directional setting,
global grid, and format-agnostic camera. OBJ reports no up axis
(`ObjAdapter.cpp:561`) and therefore defaults to Z, which is wrong for the
common Y-up OBJ. FBX reports Y but always reports `metersPerUnit = 1.0`
(`FbxAdapter.cpp:1084-1088`), ignoring the authored unit scale.

**Proposal**

1. Extend the format catalog with a `PresentationProfile`:
   - default up axis and unit policy;
   - default lighting preset (Studio PBR, Studio CAD, point cloud, transparent);
   - grid on/off and ground axis;
   - background mode;
   - point-sprite size/roundness;
   - shadow/contact-shadow default;
   - camera direction/orthographic for 2D-like formats.

2. Fix the obvious cases first:
   - **OBJ:** report Y-up by default (or add a "Source up" setting); keep the
     toolbar override. This is a one-line metadata change plus a validator
     update.
   - **FBX:** read `ufbx_scene.settings.axes.up`/unit scale where available
     instead of hardcoding Y/1.0; at minimum, document the assumption and let
     the user override units.
   - **PLY:** if the file has only points or 3DGS properties, select the point
     preset.
   - **STL/3MF/STEP/IGES/BREP:** select Studio CAD with a ground plane/shadow.
   - **glTF/FBX/USD:** select Studio PBR.
   - **Transparent materials:** select the transparent backdrop/checkerboard
     preset when the scene's blend/transmission share is significant.

3. Add a small "Presentation" section to the info panel or settings for:
   - source up axis / units;
   - lighting preset;
   - background;
   - exposure (recommendation 6).

4. Use the format catalog to share these defaults across viewer and (later)
   thumbnail provider. A new format should get a default by declaration, not by
   another switch statement.

**Why rank 8:** it is a small change with immediate perceived-quality impact,
and it fixes a real orientation bug. It also gives the Studio work a per-format
hook.

**First slice:** OBJ Y-up; PLY point preset; CAD vs PBR lighting preset.

**Full slice:** FBX units; background/point-size options; provider defaults.

**Acceptance**

- A known Y-up OBJ appears upright without manual rotation.
- An STL/STEP/3MF opens in the CAD preset with a ground plane/grid and readable
  neutral material.
- A PLY point cloud opens with colored round splats and no lighting washout.
- The user can override up axis, preset and background; the choice survives the
  session and does not corrupt the source.
- Screenshot goldens cover each preset.

**Effort:** S–M (3–5 engineer-days for the first slice).
**Risk:** low. The main risk is changing the default for existing users; use a
documented default and a visible override.

---

### 3.9 Recommendation 9 — Ground contact shadow and ambient occlusion

**Goal:** ground the object and reveal form, especially for untextured CAD and
sculpts.

**What F3D does:** optional shadow maps, SSAO, grid shadow/reflection, and a
ground grid. Its per-format config enables shadows for STL/3MF/3DS/FBX/DAE/OFF
but not glTF.

**Current Preview3D behavior:** the scene clears to `#1C1C1E`
(`D3D12ViewerPath.cpp:1167-1168`) and draws a grid
(`D3D12ViewerPath.cpp:1249-1275`); there is no ground plane and no shadow. The
shader ignores the ORM occlusion channel.

**Proposal**

1. Add an optional **ground plane/shadow catcher** under the model:
   - a cheap first slice: an analytic soft blob/contact shadow based on the
     model bounds and key direction (no extra pass);
   - the proper slice: a 1024/2048 shadow map rendered from the key light,
     sampled on the ground with PCF and a depth bias; optionally self-shadowing
     for CAD.
2. Enable it by default for CAD/untextured presets, optional for PBR and point
   clouds. Add a UI toggle and a screenshot golden.
3. Add ambient occlusion:
   - use the ORM R channel (recommendation 7);
   - optional SSAO as a quality setting after the HDR/post-process
     infrastructure exists (recommendation 6).
4. Add a transparency-friendly backdrop (checkerboard or gradient) for glass/
   alpha assets, so transparent surfaces read against a known background.
5. Keep the grid, but make the ground/shadow layer independent of it; users who
   turn off the grid should still be able to keep the shadow.

**Why rank 9:** it is the difference between a floating object and a presented
product. It is especially valuable for the CAD/STL/3MF audience and for Studio
screenshots.

**First slice:** analytic contact shadow + ORM AO sampling + checkerboard
background option.

**Full slice:** shadow map + optional SSAO.

**Acceptance**

- An object on the default ground has a soft contact shadow and no floating
  appearance.
- No shadow acne/peter-panning at typical model scales; the shadow does not
  appear in front of the object.
- The shadow can be toggled off; point clouds and transparent assets have sane
  defaults.
- The frame-time cost stays within the existing budget at 1080p/1440p.

**Effort:** M–L (2 engineer-weeks for contact shadow + AO; more for SSAO).
**Risk:** medium. Shadow bias/acne and transparency interactions need goldens
and scale tests.

---

### 3.10 Recommendation 10 — Geometry topology and material model expansion

**Goal:** open line/point/wireframe assets and improve the last mile of material
fidelity.

**What F3D does:** VTK supports points, lines, polygons, triangle strips and
multiple material lobes; F3D imports points, cameras/lights and USD points, and
offers matcap/checkerboard/unlit modes. Assimp covers line/point meshes where
the source has them.

**Current Preview3D behavior:**

- `ChunkTopology` has only `TriangleList` and `PointList`
  (`WireFormat.h:40-43`);
- glTF non-triangle primitives are skipped (`GltfAdapter.cpp:1210-1212`);
- OBJ/FBX/STEP only emit triangles; USD only emits meshes/points;
- `MaterialPayload` has base color, metallic, roughness, emissive, UV
  transform, alpha, double-sided, unlit and transmission
  (`MaterialPayload.h:60-76`); it has no clearcoat, sheen, specular, IOR,
  iridescence, anisotropy, volume or emissive strength;
- transparency is sorted per-object, not composited with OIT/depth peeling.

**Proposal**

1. Add a `LineList` topology (and, if useful, `LineStrip`) with a dedicated
   vertex layout and a line pipeline (constant or vertex color, depth bias).
   Convert GL triangle strips/fans to indexed triangles in the adapter (this
   needs no wire change). Emit lines for:
   - glTF `LINES`/`LINE_STRIP`/`LINE_LOOP`;
   - OBJ line/point elements;
   - FBX line/point geometry where ufbx exposes it;
   - USD `BasisCurves`/`NurbsCurves` as polylines;
   - STEP wires when `read_wire` is enabled.
2. Add the missing material lobes in a fixed-size, versioned payload:
   - `occlusionStrength` (or keep AO in ORM);
   - `emissiveStrength`;
   - `clearcoatFactor`/`clearcoatRoughness`;
   - `sheenColor`/`sheenRoughness`;
   - `specularFactor`/`specularColor` and IOR-derived F0;
   - optionally anisotropy/iridescence/volume later.
   Implement clearcoat as a second GGX lobe and sheen as a soft velvet lobe;
   approximations are acceptable if the badge/warning says so.
3. Improve transparency:
   - at minimum, keep sorted back-to-front blending and apply the HDR tone map
     after compositing (recommendation 6);
   - for glass/transmission-heavy scenes, add weighted blended OIT or depth
     peeling as an optional quality mode.
4. Add material inspection modes (base color, metallic, roughness, normal, UV
   checker) — useful for a viewer whose job is inspecting assets, and cheap
   once the material data is in the constant buffer.

**Why rank 10:** line/point/wireframe files are a real but smaller share of the
audience; material lobes are fidelity rather than open-rate. Do this after the
catalog/compatibility/Studio work.

**First slice:** triangle strips/fans + glTF/OBJ lines and points + line
pipeline; `emissiveStrength` + clearcoat/sheen approximation.

**Full slice:** USD curves, STEP wires, OIT/depth peeling, inspection modes.

**Acceptance**

- A glTF with `TRIANGLE_STRIP`/`TRIANGLE_FAN` renders complete geometry.
- A line-only glTF/OBJ opens and displays lines.
- A clearcoat car-paint fixture shows a distinct clearcoat highlight.
- A glass fixture composites correctly against the studio backdrop.
- Wire format version/validation tests cover the new topology and material
  fields.

**Effort:** M–L (2–3 engineer-weeks).
**Risk:** medium. Wire-protocol changes need a version bump, validator updates,
hostile-worker tests and cache identity changes.

---

## 4. Suggested sequencing

### Phase 0 — measure before changing (about 1 week)

Build `tests/compat/` with a small, licensed corpus per target format:

- use a subset of F3D's `testing/data` for reference (`duck.dae`, `cheese.igs`,
  `cheese.brep`, `cheese.xbf`, `primitives.usda`, `usd_points_rgb.usda`,
  `subdiv_cube_cc.usda`, `duckCM.tga`, `world.tga`, `grayscale.exr`,
  `shanghai_bund_1k.hdr`, etc.). Verify each fixture's license before
  committing it; otherwise generate an equivalent fixture ourselves;
- add 5–10 real-world downloads per format, including package layouts and
  misnamed files;
- run each through the worker/broker path and record `opened`, `degraded`,
  `failed`, the typed error, the warning list and time-to-first-geometry.

This corpus is the acceptance scoreboard for every recommendation. Without it,
"opens more files" is anecdotal.

### Phase 1 — foundation and quick wins

- Recommendation 1 (catalog/router/Open As)
- Recommendation 2 (compatibility policy; start with glTF + USD creases)
- Recommendation 8 (OBJ Y-up, PLY point preset, CAD/PBR presets)

These three are mostly independent of new parsers and should land first.

### Phase 2 — breadth

- Recommendation 3 (OCCT host: IGES/BREP/XBF/FCStd; STEP tolerance)
- Recommendation 4 (Assimp: DAE/3DS/OFF/AMF first)
- Recommendation 5 (USD primitives/points)

Phase 2 can run in parallel with the Studio work because it touches import code
rather than graphics code.

### Phase 3 — Studio quality

- Recommendation 6 (HDR Studio; first slice procedural IBL + neutral tone map)
- Recommendation 7 (TGA/DDS/AO first, HDR/EXR next)
- Recommendation 9 (contact shadow + ORM AO)

Start the material-sphere goldens before changing the shader.

### Phase 4 — depth

- Recommendation 10 (lines/strips/fans/points, material lobes, transparency)

### Continuous

- Fuzz each new parser (Assimp, TGA/DDS/HDR/EXR, new OCCT formats).
- Run the compatibility corpus on every change.
- Keep the hostile-worker and import-isolation suites green.
- Update `docs/FORMAT-SUPPORT.md`, the README matrix, the installer, the SBOM
  and the license notices in the same change as each new format.

---

## 5. Measurement and guardrails

### Open-rate scoreboard

For each target format, track:

- **Opened:** geometry displayed with the expected feature set.
- **Degraded:** geometry displayed with a specific approximation/warning.
- **Failed:** typed error and stage.

A useful initial target is:

- 100% of the existing eight families on their positive corpus;
- ≥90% of the new long-tail corpus opened or degraded, with every failure
  classified;
- no valid file rejected solely because of an optional visual feature.

### Visual scoreboard

Add screenshot goldens for:

- a PBR material-sphere grid (white/black/red dielectric, rough/polished metal,
  clearcoat, glass);
- a CAD part (STEP/STL) on the ground;
- a point cloud/scan;
- a transparent asset;
- an untextured mesh with vertex colors.

Compare with a tolerant MAE/SSIM metric, as the thumbnail provider already does.
Do not accept a Studio change that improves one class while regressing another
without a documented tradeoff.

### Security and performance guardrails

- Every new parser stays in the AppContainer worker/host; no new viewer links.
- Every new container gets a preflight, a job-commit budget, a cancellation
  check and a fuzz target.
- No path, network, plug-in or child-process authority is added.
- First-geometry and frame-time targets from `01-product-scope.md` remain the
  acceptance gates.
- New formats do not get Explorer thumbnails until a provider adapter exists;
  do not claim thumbnail support in the installer/README early.

### Documentation and decision updates

This proposal touches several accepted decisions and should update them
explicitly if approved:

- `docs/design/01-product-scope.md`: FR-01 extension list, explicit exclusions
  (IGES/IFC/AMF/DAE), the "other CAD formats" sentence.
- `docs/design/03-file-formats-and-ingestion.md`: support matrix, texture
  policy, sidecar extensions, error taxonomy (feature severity/report).
- `docs/design/04-rendering-and-streaming.md`: HDR target, IBL/tonemap,
  ground/shadow, line topology, material fields.
- `docs/FORMAT-SUPPORT.md`, `README.md`, `THIRD-PARTY-LICENSES.md`.
- ADR-006 (Assimp fallback), ADR-017 (STEP external documents), ADR-0048
  (TGA/DDS/HDR/EXR decode), and a new ADR for the format catalog/compatibility
  policy.

---

## Appendix A — relevant format coverage matrix

| Format | Preview3D today | F3D | Recommendation |
| --- | :---: | :---: | --- |
| glTF/GLB | yes (strict) | yes | Keep; extend extensions/topology |
| OBJ/MTL | yes (orientation risk) | yes | Fix Y-up; add TGA/DDS |
| FBX | yes | yes | Keep; read axes/units; optional fallback |
| STL | yes | yes | Keep |
| PLY | yes (mesh/points) | yes | Keep; 3DGS preset later |
| 3MF | yes (strict) | yes (Assimp) | Keep lib3mf; relax per-corner/optional |
| USD/USDA/USDC/USDZ | yes (mesh-only fast/host) | yes (meshes, points, primitives, subdiv) | Add primitives/points/creases |
| STEP/STP | yes (self-contained) | yes | Relax externals/units; add repair option |
| IGES/IGS | no | yes | Add via OCCT |
| BREP/XBF/FCStd | no | yes | Add via OCCT |
| COLLADA `.dae` | no | yes | Add via Assimp |
| 3DS/DXF/X/OFF/AMF | no | yes | Add via Assimp |
| VRML/WRL/X3D | no | partial | Optional |
| SPLAT/SPZ/3DGS PLY | no | yes | Optional, separate renderer |
| Images as 2D planes | no | yes | Optional, low effort |
| IFC/Alembic/VDB/PDAL/VTK | no | yes | Defer |

---

## Appendix B — code reference index

Use this as the starting point when executing the proposal.

**Preview3D dispatch and format metadata**

- `interactive-viewer/src/app/D3D12ImportBridge.h:28-40` — `SourceFormat` enum.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp:268-289` —
  `ToBrokerFormat`.
- `interactive-viewer/src/app/D3D12ImportBridge.cpp:425-441` —
  `ClassifyByExtension`.
- `shared/model-core/include/model_core/WireFormat.h:120-140` —
  `SourceFormatId`.
- `shared/import-broker/src/SharedSectionValidator.cpp:227-251` — per-format
  units/up-axis validation.
- `interactive-viewer/src/platform/ShellIntegration.cpp:40-43` — extension list.
- `interactive-viewer/src/app/ActiveInstance.cpp:323-326` — single-instance
  acceptance list.
- `interactive-viewer/src/app/Preview3D.cpp:2230-2238` — open-dialog filters.
- `packaging/installer/Preview3D.nsi` — ProgIDs and RegisterExtension calls.

**Preview3D parsing/limits**

- `import-worker/src/GltfAdapter.cpp:400-411` — supported extension list.
- `import-worker/src/GltfAdapter.cpp:111-118` — fastgltf error mapping.
- `import-worker/src/GltfAdapter.cpp:2399-2412` — parser extension mask.
- `import-worker/src/GltfAdapter.cpp:1210-1212` — non-triangle primitive skip.
- `import-worker/src/ThreeMfAdapter.cpp:382-390` — per-corner material failure.
- `compatibility-host-step/src/StepPart21Preflight.cpp:423-430` — external
  document substring rejection.
- `compatibility-host-step/src/StepXdeAdapter.cpp:1475-1503` — unit strictness.
- `compatibility-host-step/src/StepTessellationProfile.h:144-165` — no-healing
  policy.
- `import-worker/src/UsdAdapter.cpp:367-372` — crease rejection.
- `import-worker/src/UsdAdapter.cpp:394-400` — procedural primitives rejection.
- `compatibility-host/src/OpenUsdHost.cpp:1164-1181` — crease/subdiv rejection.
- `compatibility-host/src/OpenUsdHost.cpp:1485-1506` — mesh-only geometry.
- `shared/import-broker/src/ImportSession.cpp:1474-1488` — fallback-only-on-
  `UnsupportedComposition` rule.
- `import-worker/src/ImageFormatSniff.cpp:63-93` — decodable image extensions.
- `shared/model-core/include/model_core/PixelFormats.h:29-36` — texture formats.
- `shared/model-core/include/model_core/MaterialPayload.h:60-76` — material
  fields.
- `shared/model-core/include/model_core/WireFormat.h:40-43` — topology set.

**Preview3D rendering**

- `interactive-viewer/src/app/D3D12ViewerPath.cpp:274-304` — analytic studio
  environment / irradiance / `DisplayMap`.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:202-217, 311-401` — camera-
  space shading and PBR/transmission path.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:356-357` — metallic/roughness
  sampling (AO R channel unused).
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:1167-1168, 1249-1275` — clear
  color and grid.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp:1318-1326` — alpha sort.
- `interactive-viewer/src/app/RenderThread.cpp:61-73` — generic warning text.
- `interactive-viewer/src/app/Preview3D.cpp:2015-2017` — generic STEP warning.

**F3D reference**

- `library/plugin/reader.h:35-215` — reader contract.
- `library/src/factory.cxx.in:16-53, 89-114` — scored selection and forced
  reader.
- `library/src/scene_impl.cxx:355-412` — reader selection and error messages.
- `cmake/f3dPlugin.cmake:38-66` — reader declaration/metadata generation.
- `plugins/native/CMakeLists.txt:21-416` — native format declarations.
- `plugins/assimp/CMakeLists.txt:42-118` — Assimp format declarations.
- `plugins/occt/CMakeLists.txt:57-133` — OCCT format declarations/options.
- `plugins/usd/CMakeLists.txt:31-41` — USD declarations.
- `plugins/native/configs/config.d/10_native.json` — per-format defaults.
- `plugins/assimp/configs/config.d/10_assimp.json` — per-format defaults.
- `vtkext/private/module/vtkF3DMetaImporter.cxx:487-527` — legacy-to-PBR
  conversion.
- `vtkext/private/module/vtkF3DMetaImporter.cxx:608-643` — default light kit.
- `vtkext/private/module/vtkF3DRenderer.cxx:519-617` — render passes and
  neutral tone mapping.
- `vtkext/private/module/vtkF3DRenderer.cxx:1430-1717` — HDRI/IBL prefiltering.
- `plugins/assimp/module/vtkF3DAssimpImporter.cxx:67-160` — camera/light import.
- `plugins/assimp/module/vtkF3DAssimpImporter.cxx:1382-1449` — content sniff.
- `plugins/usd/module/vtkF3DUSDImporter.cxx:764-953` — USD primitives/points.
- `resources/defaultHDRI.png` — reference studio environment.

---

## Appendix C — proposed catalog descriptor (sketch)

```cpp
// Compile-time, header-only, no dynamic plug-ins. Worker-only importer
// implementations are referenced by an enum; the trusted viewer never links
// a parser.
enum class FormatId : uint32_t { /* append-only; existing values preserved */ };

struct MagicSignature {
    uint32_t offset;
    std::span<const uint8_t> bytes;
    std::span<const uint8_t> mask; // optional, for tolerant signatures
};

enum class ImporterKind : uint8_t {
    GltfWorker,
    StlWorker,
    PlyWorker,
    ObjWorker,
    FbxWorker,
    ThreeMfWorker,
    UsdFastWorker,
    UsdHost,
    CadHost,     // Step/Iges/Brep/Xbf/FCStd
    AssimpWorker // Dae/3ds/Dxf/X/Off/Amf/X3d
};

enum class UnitPolicy : uint8_t {
    Meters,          // glTF
    AuthoredOrMm,    // STEP/IGES
    AuthoredOrOne,   // FBX/USD
    Unknown          // STL/PLY/OBJ (display only)
};

struct PresentationProfile {
    UpAxisId upAxis;
    LightingPreset lighting;
    BackgroundMode background;
    bool grid;
    bool groundShadow;
    float pointSize;
};

struct FormatDescriptor {
    FormatId id;
    std::string_view stableName;
    std::string_view displayNameKey;
    std::span<const wchar_t* const> extensions;
    std::span<const MagicSignature> signatures;
    ImporterKind importer;
    uint8_t priority;
    UnitPolicy unitPolicy;
    PresentationProfile presentation;
    ThumbnailFamily thumbnail;
    uint32_t capabilities; // sidecars, archive, coarse proxy, stream, ...
};
```

The catalog should be **tested**, not just declared:

- every extension is unique across descriptors;
- every descriptor has a display name, installer line and docs row;
- every `ImporterKind` has a worker/host dispatch case;
- every signature fixture is accepted and a malformed fixture is rejected;
- the provider roster and installer lists are derived from the same table.

---

## Appendix D — suggested glTF required-extension policy table

| Extension | Class | Default action |
| --- | --- | --- |
| `KHR_draco_mesh_compression` | Critical | Decode; fail if undecodable |
| `EXT_meshopt_compression` | Critical | Decode; fail if undecodable |
| `KHR_mesh_quantization` | Critical | Support |
| `KHR_texture_basisu` | Texture | Decode; fallback texture if needed |
| `EXT_texture_webp` | Texture | Decode; fallback texture if needed |
| `KHR_texture_transform` | Core | Support |
| `KHR_materials_unlit` | Core | Support |
| `KHR_materials_transmission` | Visual | Approximate |
| `KHR_materials_ior` | Visual | Use for F0/transmission |
| `KHR_materials_pbrSpecularGlossiness` | Visual | Approximate |
| `KHR_materials_clearcoat` | Visual | Parse; approximate or ignore with warning |
| `KHR_materials_sheen` | Visual | Parse; approximate or ignore with warning |
| `KHR_materials_specular` | Visual | Parse; approximate or ignore with warning |
| `KHR_materials_iridescence` | Visual | Ignore with warning |
| `KHR_materials_anisotropy` | Visual | Ignore with warning |
| `KHR_materials_volume` | Visual | Ignore with warning |
| `KHR_materials_dispersion` | Visual | Ignore with warning |
| `KHR_materials_emissive_strength` | Visual | Bake into emissive factor |
| `KHR_materials_variants` | Scene | Choose the default variant |
| `KHR_lights_punctual` | Scene | Ignore for Studio; import later |
| `EXT_mesh_gpu_instancing` | Instance | Expand or warn |
| Unknown required | Unknown | Keep core geometry if present; warn; fail only if no usable geometry |

The table belongs in shared code, not in a comment or a chain of `if`s.

---

## Appendix E — decisions requested from review

| Decision | Options | Recommendation |
| --- | --- | --- |
| Scope expansion | Keep the eight-family scope; or amend FR-01/exclusions for IGES/BREP/XBF/FCStd/DAE/3DS/DXF/X/OFF/AMF | Amend for the CAD and mesh long tail; defer IFC/Alembic/VDB/PDAL/VTK |
| Assimp | Reject (ADR-006 unchanged); or add a bounded worker-only compatibility adapter | Add for formats with no native adapter; never for glTF/OBJ/FBX/STL/PLY/USD/STEP |
| Studio environment source | Procedural studio cubemap; embedded small HDRI; both | Procedural default + optional embedded/user HDRI later |
| HDR pipeline | In-shader tone map on the current target; or HDR offscreen + post pass | HDR offscreen + neutral tone map; it fixes alpha and clipping |
| New format thumbnails | Viewer only first; or block on provider adapters | Viewer first; provider adapters as a separate phase |
| Tier B limits | Keep hard ceilings; or degrade to a coarse proxy | Degrade where a trustworthy proxy can be built; keep hard ceilings for unsafe/undecodable data |

The first two decisions determine whether recommendations 3 and 4 are in scope.
The last four can be settled during implementation without changing the overall
ranking.

---

## Closing assessment

F3D is worth studying because it demonstrates that **format breadth is mostly a
metadata and compatibility problem**, not a rendering problem. It also
demonstrates the limits of that approach: its bundle is large, its parsers are
in-process, and its shell integration is not a model for ours. The winning
combination for Preview3D is:

- F3D's **catalog** and **content awareness**;
- F3D's **best-effort compatibility posture**, bounded by our security model;
- F3D's **per-format defaults** and a small set of high-value optional
  libraries (OCCT already present, Assimp for the long tail);
- our **AppContainer/broker isolation**, **normalized scene**, and **D3D12
  renderer**;
- a Studio default that goes beyond F3D: a real studio IBL, neutral tone
  mapping, exposure, ground contact and material-aware presets.

If only one thing is done first, do the **format catalog + content router +
Open As**. It is the smallest change that removes a whole class of false
failures and makes every subsequent format addition cheaper. If only one
rendering thing is done first, do the **procedural studio IBL + neutral tone
map**; it will change how every metal, glass and white material reads, without
adding a single parser.
