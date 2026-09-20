# STEP-001 OCCT, containment, and tessellation feasibility spike results

Status: **complete — go for STEP-002 with a constrained OCCT port**
Completed: 2026-09-18
Repository base: `eab29fe` (STEP proposal commit plus working tree)
Design authority: [stp.md](stp.md), [design/README.md](design/README.md)

## Result

Open CASCADE Technology (OCCT) **7.8.1** can import a self-contained
ISO 10303-21 assembly through a product-owned seekable stream over an
inherited read-only handle, inside the real zero-capability AppContainer with
a kill-on-close Job Object. XDE transfer retained assembly occurrences,
repeated definition reuse, nested locations, per-shape colors, transparency,
and length-unit metadata; `BRepMesh_IncrementalMesh` produced deterministic
bounded triangulations. Malformed bytes were rejected before OCCT ran, a
pre-signalled cancellation was observed, a Job commit ceiling terminated the
host without a false success, and a later valid import succeeded without any
viewer restart. A separate authority probe launched under the same container
was denied direct filesystem opens, network connects, and child processes.

The decision is **go for STEP-002** with five non-negotiable boundaries:

1. Input is a stream over the inherited handle only. The spike used
   `STEPCAFControl_Reader::ReadStream(const char*, std::istream&)` with a
   product-owned `std::streambuf` backed by `ReadFile`/`SetFilePointerEx`; no
   path, temporary file, current-directory lookup, or `CSF_*` resource path
   was used. The fixture directory was deliberately not granted to the
   container SID, proving handle-only access.
2. The registry `opencascade` port is **not** an acceptable closure: it builds
   visualization and every non-STEP data-exchange toolkit (48 OCCT DLLs,
   ~51.3 MB Release). A constrained overlay port
   (`packaging/vcpkg-ports/opencascade`) that disables Visualization, Draw,
   DETools, Tcl, samples, writers, OpenGL/freetype, and the non-STEP
   DataExchange toolkits is checked in and is the STEP-001 pin.
3. OCCT normalizes transferred geometry to its Cascade system unit (default
   millimetre) and records that in the XDE document. The authored Part-21 unit
   is only recoverable as a **name** (`STEPControl_Reader::FileUnits`), not as
   a factor from the document. STEP-003 must compute `metersPerUnit` from the
   authored unit explicitly rather than trusting `XCAFDoc_DocumentTool::
   GetLengthUnit`, which reports the normalized system unit.
4. OCCT's external-document resolver is **path-based and private**
   (`STEPCAFControl_Reader::ReadExternFile(file, fullpath, doc, progress)` is
   protected and receives a resolved filesystem path). There is no public
   stream/callback resolver that could be brokered through duplicated handles.
   Per [stp.md](stp.md) this is a **no-go for external STEP references**; the
   product can ship only the explicitly documented self-contained subset
   unless the product owner accepts that scope, and the AppContainer boundary
   is not weakened to gain it.
5. The spike is test plumbing. It adds no public opcode, `SourceFormatId`
   value, extension filter, shell registration, viewer route, installer claim,
   or thumbnail behavior.

## Dependency decision

| Item | Value |
| --- | --- |
| Upstream | `Open-Cascade-SAS/OCCT` |
| Release | `7.8.1` (tag `V7_8_1`) |
| Source archive SHA-512 | `807c1f8732926cfdabcfbdf8d6a0e76b8dba1a1e614afe084a467ffb4cfd80623f5e3afa7e9905b1ac96667c93e01b5f98ceaa8948a576a1093d98df98cc8f81` |
| License | LGPL-2.1 with the OCCT exception (`LICENSE_LGPL_21.txt`, `OCCT_LGPL_EXCEPTION.txt`); a commercial OCCT license is an alternative |
| Registry port | `opencascade 7.8.1#1` at baseline `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4` |
| Constrained port | `packaging/vcpkg-ports/opencascade` (same source pin; reduced modules) |
| Host manifest | `compatibility-host-step/vcpkg.json` (isolated from the root manifest) |

OCCT is deliberately kept out of the repository-root `vcpkg.json`. The root
manifest is what `Preview3DImportWorker.exe` and `Tests.ImportIsolation`
restore and autolink, so adding OCCT there would violate non-negotiable rule 1
("the trusted viewer never links OCCT"). The dedicated
`compatibility-host-step/vcpkg.json` gives the future `Preview3DStepHost.exe`
its own dependency closure, exactly as `Preview3DImportHost.exe` has its own
OpenUSD payload directory.

The spike's `compatibility-host-step/vcpkg-configuration.json` uses the local
vcpkg registry without a baseline pin because the machine's vcpkg tool and the
repository baseline `04a9d8e5` disagreed on `vcpkg_host_path_list`
(`REMOVE_DUPLICATES`). The OCCT source itself is pinned by tag and SHA-512 in
the overlay port, so the security-relevant input is fixed; STEP-002 must
re-enable the repository baseline pin once the toolchain matches and re-verify
the port ABI.

### Constrained module closure

`packaging/vcpkg-ports/opencascade/portfile.cmake` builds only
FoundationClasses, ModelingData, ModelingAlgorithms, ApplicationFramework, and
DataExchange, and rewrites `adm/MODULES` to reduce the DataExchange toolkit set
to `TKDE TKXSBase TKDESTEP TKXCAF`. It disables Visualization, Draw, DETools,
Tcl, OpenGL, freetype, TBB, rapidjson, Draco, FreeImage, VTK, FFmpeg, OpenVR,
OpenCL, jemalloc, and D3D, compiles resource lexicons in
(`BUILD_RESOURCES=ON`), and installs no samples or tests.

The constrained port was built and measured. Its Release install is **39
OCCT DLLs / 45,487,104 bytes**, versus the registry port's 48 DLLs /
~51.3 MB. `TKDEIGES.dll`, `TKDESTL.dll`, `TKDEOBJ.dll`, `TKDEPLY.dll`,
`TKDEGLTF.dll`, `TKDEVRML.dll`, `TKRWMesh.dll`, `TKDECascade.dll`, and
`TKMeshVS.dll` are gone. Three visualization-adjacent DLLs remain
(`TKV3d.dll` 2,432,000 B, `TKService.dll` 979,968 B, `TKVCAF.dll` 190,464 B)
because `TKBinXCAF` imports `TKService` and `TKXCAF` imports `TKVCAF`, and the
STEP reader's `STEPCAFControl_Provider.cxx` includes `BinXCAFDrivers.hxx`.
Removing them requires a source patch to that provider (or dropping XCAF
persistence, which the XDE reader uses); STEP-002 must decide and record it.
The host executable's direct OCCT imports are only `TKernel`, `TKMath`,
`TKBRep`, `TKDESTEP`, `TKLCAF`, `TKMesh`, and `TKXCAF`. STEP-002 must verify
the shipped DLL count and confirm `Preview3D.exe` / `Preview3DImportWorker.exe`
still load none of them.

## Stream and XDE proof

The spike host source is `compatibility-host-step/src/`:

- `StepSpikeProtocol.h` — test-only bounded shared-section and handshake
  contract, carrying only counts, units, bounds, timings, and a product-owned
  diagnostic string.
- `StepXdeSpike.cpp` — `HandleStreamBuf` over the inherited handle,
  `AdmitPart21` byte-level envelope check, `STEPCAFControl_Reader` transfer,
  bounded XDE walk, color/transparency survey, and `BRepMesh_IncrementalMesh`
  tessellation.
- `main.cpp` — `--step-001-spike <sectionHandle> <sectionSize>` entry with
  `SetDefaultDllDirectories`, `AddDllDirectory(exeDir)`, current-directory
  removal, and every `CSF_*` / `PATH` escape hatch cleared before OCCT runs.

The exact stream API used is `STEPCAFControl_Reader::ReadStream(theName,
theIStream)`, which forwards to `STEPControl_Reader::ReadStream`. No
`ReadFile`, `OSD_Path`, `CSF_OCCTResourcePath`, or temporary path was
involved. The adapter reads only the first and last 4 KiB / 64 B for the
envelope check and then streams the rest through the `streambuf`.

XDE facts observed on the AP214 assembly fixture: one free-shape root, one
assembly label, five shape labels, two unique definitions, five component
instances (three reused references), depth 2, three colors (two transparent),
24 faces, 84 edges, 596 triangles, 644 extracted vertices, `metersPerUnit =
0.001` (millimetre). AP203 (`CONFIG_CONTROL_DESIGN`) and AP242
(`AP242_MANAGED_MODEL_BASED_3D_ENGINEERING_MIM_LF`) analytic parts each
produced 7 faces / 30 edges / 84 triangles and one color. The inch fixture
declared `CONVERSION_BASED_UNIT('INCH', ...)`; the adapter reported the
authored unit name `INCH` while the XDE document reported the normalized
millimetre system unit, confirming finding 3 above.

## Sandbox and authority proof

A standalone harness (`StepSpikeSandboxHarness.cpp`) launched
`Preview3DStepSpike.exe` through the product's real
`import_broker::LaunchSuspendedSandboxed` with a zero-capability AppContainer
token, a kill-on-close Job Object, and an explicit
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` containing only the control pipes, the
read-only source handle, the cancellation event, and the output section. The
container SID was granted read+execute on the host payload directory only; the
fixture directory was not granted. Results:

| Case | Observed |
| --- | --- |
| Valid AP214 assembly | `Success`; 5 instances, 2 reused definitions, 596 triangles, transparency preserved |
| Malformed non-Part-21 bytes | `PreflightFailure` with no OCCT transfer (`read_us=0`) |
| Pre-signalled cancellation | `Cancelled` before transfer |
| 4 MiB Job commit ceiling | host terminated with no reply and no false `Success`; replacement host imported `part_ap203.stp` successfully |
| Authority probe (same container) | `file_denied=1 network_denied=1 process_denied=1` |

The host executable was built as a console subsystem binary and produced no
visible window; the harness terminated each generation's Job Object after
inspection. The spike does not claim that OCCT itself is cancellation-safe
inside a long transfer or meshing call; the Job Object remains the hard
backstop, consistent with [stp.md](stp.md).

## Fixtures

Generated with OCCT's XDE writer by a test-only generator
(`GenerateStepFixtures.cpp`); the product never links the writer. All files
begin with `ISO-10303-21;` and end with `END-ISO-10303-21;`.

| Fixture | Bytes | SHA-256 | Coverage |
| --- | ---: | --- | --- |
| `part_ap203.stp` | 21,167 | `8359D9565CDC6EE5FC580A7796D5597AA12B2152BA2132539201606376156B96` | AP203 `CONFIG_CONTROL_DESIGN`, analytic solid with through-hole, color, millimetre |
| `part_ap214.stp` | 19,632 | `8ABCCA959CE556B2D1B8E20775D544E3E5E8DC6694316B501F3B7E2446FA5BFA` | AP214 `AUTOMOTIVE_DESIGN`, same geometry, millimetre |
| `part_ap242.stp` | 19,664 | `B28DB38DA9DEE8E1F075171094AF3935A56A3934A9A2C895165D7ECB493A6967` | AP242 managed model based 3D engineering |
| `assembly_ap214.stp` | 25,554 | `649F99A5891010B685CF5634A03AEBAD9537081D836659161A59A8A195E3AB0F` | Shared definition reused twice, cylinder, nested transform, face colors, transparent color |
| `inch_part_ap214.stp` | 19,917 | `29758B701A5550A40743910A5B392340C1673E8A8CE985BED96236DFF83CD0C7` | `CONVERSION_BASED_UNIT('INCH')` authored unit |

The corpus deliberately excludes PMI/GD&T, external references, and
tessellated AP242 representations; those remain STEP-003/004/005 work and are
asserted there against immutable fixtures.

## Measurements

Environment: Windows 11 Pro 10.0.26200, AMD Ryzen 9 7900X3D (24 logical
processors), MSVC 14.51, Release OCCT 7.8.1, x64. The measurements used the
registry port build; the constrained port is the same source and public API
with unneeded modules removed, so the STEP/XDE/tessellation path is unchanged.
Values are single diagnostic runs, not release benchmarks. `private` / `peak`
are host process private bytes and lifetime peak working set; the peak is
monotonic across a reused process. The in-process driver numbers include host
startup.

| Fixture | Read | Transfer | Walk | Mesh | Total | Triangles | Private | Peak WS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `part_ap203.stp` | 1.57 ms | 3.15 ms | 3.58 ms | 3.56 ms | 10.7 ms | 84 | 6,037,504 | 21,180,416 |
| `part_ap214.stp` | 1.89 ms | 3.34 ms | 3.61 ms | 3.60 ms | 12.1 ms | 84 | 5,537,792 | 20,926,464 |
| `part_ap242.stp` | 1.94 ms | 4.83 ms | 3.92 ms | 3.91 ms | 13.4 ms | 84 | 5,689,344 | 21,024,768 |
| `assembly_ap214.stp` | 1.60 ms | 3.00 ms | 8.99 ms | 8.94 ms | 15.7 ms | 596 | 6,414,336 | 20,877,312 |
| `inch_part_ap214.stp` | 2.05 ms | 5.18 ms | 4.90 ms | 4.88 ms | 15.1 ms | 84 | 5,603,328 | 21,061,632 |

In the real AppContainer the same assembly import completed in 14.3 ms with
6,402,048 private bytes. The 4 MiB Job ceiling terminated the constrained host
before it could reply, and the replacement import completed in 9.6 ms. These
are order-of-magnitude Tier-B values on tiny fixtures; a large industrial
assembly, pathological trimmed surfaces, and the coarse/display split remain
STEP-004/007 measurements and are not promised here.

## Findings that change the plan

1. **Units need explicit product handling.** OCCT converts to the Cascade
   system unit and sets the document unit to it, so
   `XCAFDoc_DocumentTool::GetLengthUnit` returns the *normalized* unit, not the
   authored one, and `StepData_StepModel::LocalLengthUnit` is normalized too.
   The authored unit is available by name through
   `STEPControl_Reader::FileUnits`. STEP-003 must map that name (or the
   model's unit entities) to a verified `metersPerUnit` and decide whether to
   retain OCCT's normalized geometry or re-scale; a mixed or unresolvable unit
   must fail rather than assume millimetres.
2. **External references are a no-go.** The resolver is path-based and
   protected; there is no stream/callback hook. STEP-005 must either be
   re-scoped to the self-contained subset with product-owner sign-off or
   remain blocked. The AppContainer boundary must not be weakened.
3. **Healing was not enabled.** The spike used OCCT defaults and no
   `ShapeFix`/`XSAlgo` healing sequence. The healing comparison required by
   [stp.md](stp.md) remains open; the initial release should keep broad
   automatic healing off and fail invalid geometry rather than mutate it.
4. **Cancellation is phase-granular.** Cancellation was observed before
   transfer, but OCCT offers no per-face or per-transfer progress hook in the
   path used. The Job Object is the only hard stop inside an uninterruptible
   transfer or meshing call, exactly as the design already states.
5. **Threading was single-reader.** The spike used one reader, one document,
   and one host process per import. No concurrency claim is made.

## Verification

The spike harness passes 5 cases / 21 checks in Release:

```text
[1] valid AP214 assembly through inherited handle only      PASS
[2] malformed non-Part-21 rejected before OCCT transfer     PASS
[3] pre-signalled cancellation observed                     PASS
[4] Job commit ceiling terminated; replacement recovered    PASS
[5] path/network/child-process denial                       PASS
```

The fixtures are generated, not downloaded, so the corpus is reproducible
without network access once OCCT is restored.

## STEP-002 handoff

STEP-002 may proceed. It must:

- build `Preview3DStepHost.exe` as a real project with the constrained
  `compatibility-host-step` closure and prove `Preview3D.exe` /
  `Preview3DImportWorker.exe` / the thumbnail provider load none of it;
- add `SourceFormatId::Step`, `ImportFormat::Step`,
  `ControlOpcode::StartStepImportFromFile`, and `ParseStepFileRequest`, and
  wire the broker/session/validator/cache paths;
- replace `AdmitPart21` with the full bounded `StepPart21Preflight` lexical
  scanner and keep it ahead of the OCCT reader;
- keep the `HandleStreamBuf`, handle-only input, `CSF_*`/PATH clearing, and
  copy-then-validate rules; and
- carry the unit, external-reference, cancellation, and healing findings above
  into STEP-003/004/005 rather than re-deriving them.
