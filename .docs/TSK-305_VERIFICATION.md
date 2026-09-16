# TSK-305 release acceptance and scope handoff

Date: 2026-09-16

## Outcome

Release acceptance was executed against commit
`eccd8bae8167f38f0e224e989a99a93484c6549e` plus the packaging allowlist fix
recorded by this task. The scope-limited MVP is **not release-ready**. The
functional, isolation, lifecycle, accessibility, small-scene responsiveness,
and local portable-package lanes pass, but retained release rows remain blocked:

1. The generated 3,000,000,084-byte STL still presents no geometry or complete
   coarse proxy in 30 seconds. TSK-302's equivalent multi-GiB GLB and PLY
   failures are not waived, and A-medium remains unqualified.
2. The live budget matrix now reproducibly times out before complete-proxy
   publication for the 8,000,000-point little-endian PLY in both fresh discrete
   and simulated-UMA processes. Each hit the explicit 180-second bound with
   zero queued upload bytes. The fixture SHA-256 is
   `abb00593050a788e973ab101850424817b9d3007e95d692933bbdd42c3992fb2`.
3. No 144 Hz/high-performance reference run, physical UMA reference, real ETW
   PresentMon capture, physical unlike-DPI monitor crossing, or assistive-
   technology speech review was available. These rows remain unverified, not
   inferred from WARP, simulation, or a 60 Hz desktop.
4. No signing certificate or clean offline Windows 11 standard-user VM was
   available. The final local archive is deliberately marked unsigned.

No failed or unavailable row is reported as passed. Fixing a blocker requires
rerunning the affected lane and recreating the archive; it is not a documentation
waiver or an implicit scope change.

## Post-acceptance scan-throughput remediation

The subsequent protocol-v9 and bounded-parser optimization pass preserves the
accepted MVP architecture and limits; it does not waive or broaden them. On the
same local compatibility machine, three fresh Release processes now reach
complete coarse at 4,185.989 ms p95 for the retained 3.00 GB STL, 4,667.902 ms
p95 for the 2.88 GB little-endian point PLY, and 5,217.987 ms p95 for the
4.29 GB GLB. First geometry remains early at 539.466, 506.841, and 511.429 ms
p95. Corpus A-small complete coarse is 466.718 ms p95, and a separate 8 MiB
A-small copy is 491.910 ms p95. Thus the original no-geometry
and >11-second STL/PLY blockers are fixed, but GLB remains 217.987 ms over the
five-second complete-coarse target and the STL run still misses the separate
frame-interval gate. Official high-performance-reference, unavailable hardware,
signing, clean-VM, and package recreation rows remain open.

The design-visible change is explicitly recorded in
`design/03-file-formats-and-ingestion.md`: protocol v9 carries compact scan
summaries, a validated deindexed hint, and the new split-invariant wire checksum.
Viewer and worker still ship together and reject every other protocol version;
hostile-worker coverage was updated rather than adding mixed-version migration.
Final Debug/Release solution builds pass; Unit passes 92 cases / 7,360 Debug
and 7,272 Release assertions, and ImportIsolation passes 200 cases / 52,068
assertions in each configuration.

## Final build and automated suites

Both solution configurations were rebuilt from scratch:

```powershell
msbuild Preview3D.slnx /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
msbuild Preview3D.slnx /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Both completed with zero reported warnings/errors. The freshly rebuilt Catch2
binaries produced:

| Suite | Result |
| --- | --- |
| Unit Debug | 91 cases / 7,354 assertions passed |
| Unit Release | 91 cases / 7,266 assertions passed; expected unavailable-debug-layer warnings |
| ImportIsolation Debug | 200 cases / 51,943 assertions passed |
| ImportIsolation Release | 200 cases / 51,943 assertions passed |

The ImportIsolation total includes the hostile worker, AppContainer/Job/handle
policy, copy-then-validate, malformed/overflow, source-change, sidecar,
cancellation, stale-generation, timeout, and recovery cases. TSK-301 is the only
post-TSK-209 control-protocol change; this final run therefore reruns the hostile
suite against the current protocol and all enabled adapters.

Debug and Release activation, accessibility, and lifecycle automation passed.
The final Release deep lanes also passed bounded progressive queues, delayed
publication/cancellation, low-mip-first textures and fallback, typed hostile
input/error recovery, graphics reconstruction, complete-coarse handoff, fine
eviction, and zero-worker cleanup. The coarse lane exercised GLB, STL, both
endian PLY meshes and points, and Draw-heavy with no D3D12 validation errors.
The budget lane passed pressure/eviction/recovery through both-endian 2M-triangle
PLY meshes before hitting the independently reproduced 8M-point blocker above.

## Local performance evidence

This machine is Windows 11 Pro build 26200, Ryzen 9 7900X3D, 64 GiB RAM, NVMe,
RTX 4080 driver 32.0.16.1664, 3840 x 2160 at 60 Hz, Balanced power. It is useful
compatibility/development evidence but does not satisfy the documented 144 Hz,
high-performance reference definition.

Three fresh Release compatibility processes for A-small
(`SHA-256 412f9110575c0b425d8cdc4726ca5a59c4e91c09de4f3d56a226fd1f2fbb6240`)
all passed applicable gates:

| Metric | p95 / maximum |
| --- | ---: |
| First background/loading UI | 454.727 ms / 454.727 ms |
| First geometry/complete coarse | 456.796 ms / 456.796 ms |
| Frame interval | 4.391 ms / 17.315 ms |
| Synthetic input-to-present | 4.619 ms / 4.619 ms |
| Viewer private commit | 290,811,904 bytes / 290,811,904 bytes |
| Worker private commit | 3,584,000 bytes / 3,584,000 bytes |

The startup number is retained but is not claimed against the performance-only
150/200 ms gate on this non-reference run. One Draw-heavy run passed its
applicable compatibility gates with 4.535 ms frame p95, 17.925 ms maximum, and
3.584 ms input-to-present. The final 3 GB STL negative result retained a 4.387
ms frame p95 and 20.743 ms maximum, with 407,711,744 viewer and 36,204,544
worker peak private bytes, but had no first geometry or complete coarse milestone.
Responsive empty presentation does not satisfy progressive usefulness.

## Retained acceptance matrix

| Retained row | Result | Evidence / remaining requirement |
| --- | --- | --- |
| Supported content | Pass locally | Expected metadata/format suites, lifecycle corpus, texture/readback tests, both-endian mesh/points, meshopt/WebP and typed unsupported-required-feature behavior pass. |
| Startup | Blocking: reference evidence unavailable | Loading UI is asynchronous, but the required cold 144 Hz/high-performance reference measurements were not run. |
| Progressive usefulness | Blocking: failed/incomplete | A-small passes; A-medium is unrun; multi-GiB GLB/STL lack useful geometry and PLY has the retained named failure. |
| Loading responsiveness | Pass on compatibility; reference row open | Current small/Draw-heavy/large-STL compatibility intervals and input latency pass. Real 144 Hz ETW evidence remains unavailable. |
| Ready interaction | Blocking: large behavior unavailable | Small and Draw-heavy applicable checks pass; there is no successful large proxy/resident-detail run to establish the retained large-scene row. |
| Bounded CPU/upload memory | Pass on exercised large runs | Current and TSK-302 multi-GiB runs remain under the aggregate cap; mapped views are separate, queues stay bounded, and allocation/Job hostile tests pass. This does not excuse missing geometry. |
| Out-of-core GPU behavior | Blocking: partial/failure | Mesh pressure, fence-safe retirement and refinement recovery pass; the 8M-point path times out and no physical UMA reference was available. |
| Cancellation/recovery | Pass locally | Cooperative/forced cancellation, replace/close cleanup, stale filtering, typed worker/import errors and one-shot graphics reconstruction pass. |
| Existing UX/accessibility | Blocking: hardware/manual matrix incomplete | Automated keyboard/UIA, contrast, reduce-motion, 100/150/200% layout, Snap/system menu/fullscreen and activation pass; physical unlike-DPI and screen-reader speech review remain open. |
| Isolation/visual correctness | Pass for automated release suite | Full hostile/malformed/security suites and debug visual/readback lanes pass. No sustained external sanitizer fuzz campaign was run beyond the checked-in malformed/fuzz regression corpus. |
| Portable delivery | Blocking: unsigned and no clean VM | Local Unicode extraction, offline-capable dependency closure, Tier A/compressed texture smokes, hashes and cleanup pass; signed clean-standard-user offline VM evidence is absent. |

## Portable archive and packaging fix

The first post-TSK-304 packaging attempt correctly failed closed because the
viewer's new Windows inbox imports were missing from the system-DLL allowlist.
`bcrypt.dll`, `oleacc.dll`, and `uiautomationcore.dll` were added; no binary is
staged for these operating-system components. The successful command was:

```powershell
msbuild Preview3D.slnx /t:CreatePortableRelease /p:Configuration=Release /p:Platform=x64 /v:minimal
```

The resulting engineering archive is:

```text
artifacts\portable\Preview3D-0.1.0-portable-x64.zip
SHA-256 c1938aeaf67d7792d2b27f5e50a7bd56383c7ca3c303ae6242b1e3994c582c7b
signed false
31 staged files; 30 entries in MANIFEST.json
```

The staged viewer SHA-256 is
`3b91bda17029645118271ef4c0034cb35fba77a7b3e131a8bce013b529e0dadf`;
the worker is
`12854ea424a9003646f5b313f8fcbab8bb6832cfd5b6f75d7739faebdf32cf3a`.
Every manifest entry and the adjacent ZIP checksum were independently verified.
The SBOM has nine components, eight installed-port license records are present,
and none of the test, hostile-worker, thumbnail, compatibility-host, symbol, or
library payloads entered the archive.

The archive was extracted beneath a Unicode directory and its packaged viewer
opened GLB, glTF with binary sidecar from a separate Unicode directory, binary
STL, both-endian PLY meshes and points, meshopt GLB, and WebP glTF. All nine
processes returned status `pass`; first geometry ranged from 367.744 to 418.434
ms. No viewer/worker remained. The included profile cleanup removed its
current-user ACL/profile state and passed again idempotently.

This local medium-integrity run is not represented as a clean-machine or signed
candidate result. Network-client source review finds no product networking path;
the only socket/connect calls are the deliberate worker containment probe, whose
tests prove the zero-capability AppContainer cannot use the attempted connection.
Formal offline proof remains part of the missing clean-VM row.

## Dependency, license, fuzz, and threat review

- `vcpkg.json` and the pinned registry baseline have not changed since the
  TSK-209 dependency/threat review. The enabled third-party code remains confined
  to the worker and is reconciled to the package SBOM and notices.
- TSK-304 added only inbox Windows APIs. The strict PE-import closure now names
  those imports and continues to reject unknown non-system dependencies.
- All checked-in malformed/fuzz seeds and hostile-worker mutations pass through
  the current worker/validator boundary in both configurations. Aggregate image,
  parser, queue, Job and shared-section limits remain covered. A long-running
  external ASan/libFuzzer campaign was not available and is not silently claimed.
- The package introduces no network capability, registration, thumbnail,
  compatibility-host, cache, Tier B parser, or model-derived persistent write.

Machine-readable final summaries are frozen in
`tests/fixtures/baselines/tsk-305/acceptance.json`; detailed local logs and raw
benchmark intervals are under `artifacts/tsk-305/` and can be regenerated with
the commands above.
