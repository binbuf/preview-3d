# STEP-002 verification: host, Part-21 admission, and closed protocol route

Status: **complete — proceed to STEP-003**
Completed: 2026-09-19
Design authority: [stp.md](stp.md), [design/README.md](design/README.md)
Spike basis: [STEP-001-SPIKE-RESULTS.md](STEP-001-SPIKE-RESULTS.md)

## Result

The production containment and dispatch boundary for the bounded static
STEP/STP preview subset now exists. A dedicated, zero-capability
`Preview3DStepHost.exe` receives an already-open read-only source handle
through the broker, runs product-owned ISO 10303-21 admission, and emits only
the normalized protocol-v10 wire contract. No public picker, drag/drop,
Open With, installer, or thumbnail path recognizes `.step`/`.stp` yet: the
route is reachable only by the broker with `ImportFormat::Step`.

The trusted viewer and the general import worker link and load none of the
OCCT closure. OCCT lives only in the private `StepHost` payload directory.

## Closed protocol identities (protocol v10, additive)

| Identity | Value | Notes |
| --- | --- | --- |
| `model_core::SourceFormatId::Step` | 13 | A STEP generation may emit only this format. |
| `import_broker::ImportFormat::Step` | appended | Never dispatched to the general worker. |
| `model_core::ControlOpcode::StartStepImportFromFile` | 20 | Accepted only by `Preview3DStepHost.exe`. |
| `model_core::ParseStepFileRequest` | 48 bytes | Same brokered raw-file-handle shape as the other file requests. |
| `ImportErrorCode::StepHostFailure` / `StepHostLimit` | 26 / 27 | Host plumbing/crash/timeout vs. bounded resource family. |
| `import_broker::ImportProducer::StepHost` | 3 | Closed producer identity; broker enforces producer/format pairing. |

`SceneMetadata` remains 48 bytes and protocol v10 layout is unchanged; no
existing fixture, static assertion, or hostile-worker case changed meaning.

## Host and containment

- `compatibility-host-step/Preview3DStepHost.vcxproj` builds
  `x64/<Config>/StepHost/Preview3DStepHost.exe` with the constrained
  `compatibility-host-step` OCCT closure (`opencascade` overlay port, 39
  Release DLLs). The project uses its own manifest and
  `VcpkgManifestInstall=false` so an ordinary solution build never rebuilds
  the CAD kernel; STEP-003 re-enables the repository baseline pin once the
  toolchain matches.
- The broker creates the machine-wide AppContainer identity
  `Binbuf.Preview3D.StepHost` and grants it read+execute on the private
  `StepHost` directory only. The host runs suspended, under a kill-on-close
  Job Object with an explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, and exits
  after one generation (OCCT global state and peak B-rep memory are discarded
  deterministically).
- `HardenProcessDiscovery` clears every `CSF_*`/`PATH` escape hatch and pins
  DLL discovery to system32 plus the executable directory before any resource
  is consulted. The test-only `--probes` route proves direct filesystem opens,
  network connects, and child-process creation are denied under the real
  container.
- `Preview3D.exe` and `Preview3DImportWorker.exe` report zero OCCT imports
  (`dumpbin /dependents`).

## Part-21 admission

`StepPart21Preflight` is a bounded streaming lexical scanner, not a second
STEP implementation. It reads only through the inherited handle and:

1. verifies the ISO 10303-21 envelope and bounded HEADER/DATA/ENDSEC/
   END-ISO-10303-21 structure;
2. counts entity records, references, DATA sections, parenthesis nesting,
   record/string lengths, and total lexed bytes with checked arithmetic;
3. rejects binary/UTF-16/compressed/XML encodings, missing terminators,
   duplicate/zero/impossible entity identifiers, and unterminated
   strings/comments;
4. extracts the first `FILE_SCHEMA` name for classification only, never as an
   extension-based trust decision;
5. flags `FILE_POPULATION`/`DOCUMENT_FILE` external declarations. STEP-005 is a
   documented no-go, so any declaration returns
   `UnsupportedRequiredFeature` rather than being silently omitted.

The production OCCT reader cannot be reached before admission succeeds. Until
STEP-003 wires the XDE adapter, the accepted request produces a bounded,
fully validated synthetic scene (one reusable cube geometry, one node, one
mesh instance) in the STEP format.

## Failure mapping

- Host-reported admission/transfer facts are preserved
  (`MalformedData`, `UnsupportedEncoding`, `UnsupportedRequiredFeature`,
  `EmptyGeometry`, ...); their resource family normalizes to `StepHostLimit`.
- Broker launch/crash/timeout/validation faults collapse to `StepHostFailure`.
  Raw kernel messages, paths, and source strings never cross the control
  channel or reach the UI.

## Evidence

`x64/<Config>/Tests.ImportIsolation.exe "[step-002]"` passes 13 cases / 155
assertions in Debug and Release:

```text
[step-002][protocol]   additive identities and error range
[step-002][preflight]  envelope, strings/comments, malformed/encoding negatives,
                       lexical ceilings, external declarations
[step-002][validator]  unknown up axis + positive metre factor only
[step-002][step-host][sandbox]      valid Part-21 through the dedicated route
[step-002][step-host][admission]    malformed/external rejected before transfer
[step-002][step-host][cancel]       pre-signalled cancellation observed
[step-002][step-host][recovery]     crash/hang/stale/wrong-format/unknown-error/
                                    over-allocation typed and recovered in-process
[step-002][step-host][containment]  path/network/child-process denial
[step-002][viewer-bridge]           explicit Step route; discovery stays private
```

The full import-isolation suite has pre-existing USD failures unrelated to
STEP (fixture SHA-256 review and the legacy `--usd-002-spike` host route fail
identically on the STEP-002 baseline with these changes stashed).

## Handoff to STEP-003

- Add `StepXdeAdapter` behind the existing `StartStepImportFromFile` route,
  replacing the synthetic placeholder with the accepted XDE traversal.
- Carry the STEP-001 unit finding: compute `metersPerUnit` from the authored
  unit explicitly; `UpAxisId` stays `Unknown`.
- Keep broad automatic healing off; fail invalid geometry rather than mutate
  it.
- Re-enable the repository vcpkg baseline pin for the constrained OCCT port
  and re-verify its ABI once the toolchain matches.
