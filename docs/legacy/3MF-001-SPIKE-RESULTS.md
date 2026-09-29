# 3MF-001 dependency and feasibility spike results

Status: **complete — go for 3MF-002**

Completed: 2026-09-18

Repository base: `da0dbb1fe086d5ace2c49f4283cd6bf3b054cba4`

## Result

lib3mf 2.5.0 can read the approved static 3MF subset from an inherited file
handle inside the existing zero-capability AppContainer worker. Core,
components, Production parts, Materials/texture resources, and Beam Lattice
resources loaded in strict mode. Callback I/O, deterministic extraction,
cooperative cancellation, Job containment, and worker replacement all passed
in the real worker pool.

The result is a go for 3MF-002 with four non-negotiable boundaries:

1. Product-owned OPC preflight still runs before lib3mf and owns ZIP64,
   streaming-package, path, relationship, count, expanded-byte, compression,
   and required-extension policy.
2. lib3mf progress callbacks cover package/model load phases, not mesh/lattice
   getters or product normalization. Those loops need explicit cancellation
   checks; a 500 ms grace followed by Job termination remains the hard stop.
3. A valid authored Beam Lattice representation mesh is preferred. Otherwise
   the product performs bounded, chunked tessellation. Reusable templates are
   only valid for the narrow equal-radius/no-clipping/spherical-cap case.
4. The spike is private test plumbing. It adds no public opcode, source-format
   value, extension filter, shell registration, viewer route, installer claim,
   or thumbnail behavior.

## Dependency decision

The repository's existing vcpkg baseline
`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4` resolves the following closure:

| Port | Version | Purpose / license source |
| --- | --- | --- |
| `lib3mf` | `2.5.0#1` | Reader and object model; BSD-2-Clause |
| `libzip` | `1.11.4` | OPC ZIP access; BSD-3-Clause |
| `zlib` | `1.3.2#2` | Deflate; zlib license |
| `bzip2` | `1.0.8#6` | Stock libzip `bzip2` feature; BSD-style |
| `cpp-base64` | `V2.rc.08` | lib3mf dependency; zlib-style |
| `fast-float` | `8.2.10#1` | numeric parsing; Apache-2.0/MIT |

The lib3mf port tree is
`d6d8ac8b598ef28c1fd4d8cddea87b2807b89570`. It downloads the `v2.5.0`
release whose upstream Git commit is
`64bb454d1fcb53effa57d3cef752a10d740d41a2`; the vcpkg source archive SHA-512
is
`ACFD0E4862248C475C674F7EE7855F809965A854E62EA0CD847008BE7A9CA3C5A03AC87CAC889F036555229762405094CA9811817DD45DBDAAE941B5B41AE356`.

The x64-windows triplet selects app-local dynamic linkage. The port deletes
upstream vendored libraries and configures:

- `LIB3MF_BUILD_SHARED=ON`;
- system/pinned libzip and zlib required;
- included zlib, libzip, SSL, cpp-base64, and fast-float disabled;
- coverage and lib3mf tests disabled.

The upstream build has no supported reader-only, writer-off, bindings-off, or
per-extension compile switch. The spike calls only the C++ reader surface and
ships no bindings, examples, or tests, but the shared library still contains
the other compiled APIs. Secure Content remains outside product policy and
3MF-002 must reject it before general load. An invasive source fork merely to
remove unreachable exports was rejected because it would enlarge the patch and
update surface without reducing worker authority.

All installed ports provide vcpkg copyright/SPDX material. The fixture corpus
also carries the upstream BSD-2-Clause text. 3MF-006 must add the six enabled
ports to package notices/SBOM generation; 3MF-001 deliberately does not alter a
shipping package that does not advertise 3MF.

### Runtime closure

The additional Release app-local closure is:

| File | Bytes | Direct dependency evidence |
| --- | ---: | --- |
| `lib3mf.dll` | 2,207,232 | imports `zip.dll` and `z.dll` |
| `zip.dll` | 114,688 | imports `bz2.dll`, `z.dll`, and Windows BCrypt |
| `z.dll` | 91,648 | CRT/system only |
| `bz2.dll` | 75,776 | CRT/system only |

`Preview3DImportWorker.exe` imports `lib3mf.dll`. PE dependency inspection
confirmed that `Preview3D.exe` and `Preview3DThumbnailProvider.dll` do not
import lib3mf, libzip, zlib, or bzip2. The dependency therefore remains in the
untrusted worker closure.

## Harness and corpus

The private `--3mf-spike-pool` mode uses the ordinary `WorkerPool`, AppContainer
profile, Job, inherited shared section, inherited cancellation event, and
duplicated read-only source handle. It never receives or reconstructs a source
path and never extracts package content to disk. The mode is deliberately not
part of the product control protocol.

The read callback checks volume/file identity, file size, and last-write time
before every read; it implements exact reads, zero-fills and records any short
read, and uses checked 64-bit seeks. A deterministic identity-mismatch mode
exercises source-change rejection. lib3mf's callback ABI cannot return a byte
count or error, so the callback records the failure out-of-band and makes the
requested buffer deterministic; acceptance is denied after the library call.
This behavior must be retained in the product adapter.

The checked-in, base64-transported corpus and hashes are documented in
`tests/fixtures/3mf-spike/README.md`. It contains upstream lib3mf 2.5.0 Core,
Production, texture, simple lattice, and positive representation-mesh cases.
A 128-level component graph, a near-2 GiB sparse package, and a roughly 48 MiB
allocation-pressure model are generated at test time so large artifacts are
not committed.

## Measurements

Environment: Windows 11 Pro 10.0.26200, AMD Ryzen 9 7900X3D, 67,810,054,144
bytes RAM, MSVC 19.51.36257, MSBuild 18.10.1. Times are single diagnostic runs,
not release benchmarks. Memory values are worker process private bytes and
process peak working set; the peak is lifetime-wide and therefore monotonic
within a reused worker.

### Debug

| Fixture | Load | Extract | Private before | Private after extract | Peak working set | Normalized hash |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Core box | 2.527 ms | 0.185 ms | 14,999,552 | 15,290,368 | 16,793,600 | `a0f076d4e288f0a7` |
| Nested components | 2.661 ms | 0.161 ms | 15,294,464 | 15,302,656 | 17,178,624 | `a09f10b4927d6895` |
| Production parts | 46.811 ms | 0.127 ms | 15,302,656 | 17,022,976 | 19,079,168 | `b3c22a90cec2dace` |
| Materials/texture | 13.076 ms | 0.350 ms | 16,646,144 | 16,883,712 | 19,673,088 | `80765f68f50adc85` |
| Beam fallback | 1.870 ms | 2.532 ms | 16,883,712 | 17,022,976 | 19,673,088 | `e5e6522379d8b2cc` |
| Beam representation | 1.749 ms | 3.110 ms | 17,022,976 | 17,022,976 | 19,673,088 | `9cb5132d2d692aec` |

The generated 128-level graph loaded in 10.934 ms and ended at 16,637,952
private bytes. The same normalized hashes repeated three times and matched
Release.

### Release

| Fixture | Load | Extract | Private before | Private after extract | Peak working set | Normalized hash |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Core box | 1.635 ms | 0.070 ms | 2,727,936 | 3,026,944 | 11,788,288 | `a0f076d4e288f0a7` |
| Nested components | 1.066 ms | 0.055 ms | 3,031,040 | 3,031,040 | 12,005,376 | `a09f10b4927d6895` |
| Production parts | 7.745 ms | 0.043 ms | 3,035,136 | 4,722,688 | 13,303,808 | `b3c22a90cec2dace` |
| Materials/texture | 2.540 ms | 0.105 ms | 3,608,576 | 4,145,152 | 13,549,568 | `80765f68f50adc85` |
| Beam fallback | 1.127 ms | 0.450 ms | 4,100,096 | 4,116,480 | 13,549,568 | `e5e6522379d8b2cc` |
| Beam representation | 0.940 ms | 0.146 ms | 4,100,096 | 4,124,672 | 13,549,568 | `9cb5132d2d692aec` |

The generated 128-level graph loaded in 2.357 ms and ended at 3,608,576
private bytes.

### Source size, allocation, and containment

- A valid Core ZIP was relocated behind an NTFS sparse prefix to a total
  length of 2 GiB minus 64 KiB. It loaded successfully, performed recorded
  sparse/backward seeks, and read less than 1 MiB rather than mapping or reading
  the declared source size.
- Under a deliberately tiny 20 MiB Job commit limit, a stored package with 1.5
  million vertex elements could not complete. lib3mf reported generic error 5;
  the worker stayed contained, was explicitly retired, and a replacement
  loaded the Core fixture successfully.
- lib3mf owns ZIP/XML/model allocations during `ReadFromCallback`. The progress
  callback can abort at its phase boundaries, but only the Job is a hard
  allocation backstop. Product extraction/tessellation allocations are
  count-checked and cancellable between mesh, beam, and ball operations.

## Cancellation boundary

Returning abort from lib3mf's progress callback produced error 10
(`CALCULATIONABORTED`) and the closed `Cancelled` status at each observed load
phase:

| Phase | Covered work |
| --- | --- |
| `EXTRACTOPCPACKAGE` | central-directory/package extraction |
| `READROOTMODEL` | root XML load |
| `READNONROOTMODELS` | Production part load |
| `READRESOURCES` | resource/model construction |
| `READTEXTURETACHMENTS` | texture attachment load |

lib3mf exposes no progress callback from mesh getters or Beam Lattice access.
The spike checks the inherited event between meshes and individual beams/balls.
For a generated model with 250,000 extra vertices, cancellation injected
immediately after the non-cooperative `GetVertices` call was observed at the
next product boundary after 17.544 ms Debug / 3.211 ms Release. Cancellation
injected after the first Beam Lattice getter was observed after 0.124 ms Debug /
0.041 ms Release. A deliberately noncooperative post-load operation did not
acknowledge cancellation. The broker's 500 ms wait then terminated and replaced
it in 514 ms Debug / 516 ms Release, and the next Core load succeeded. No result
should claim more prompt cancellation than these boundaries provide.

## Beam Lattice strategy

The positive upstream fixture proves lib3mf exposes a valid authored
representation mesh. Product order for 3MF-005 is:

1. Validate and use `representationmesh` when it resolves to a reachable mesh
   within ordinary geometry/index/budget limits.
2. Otherwise tessellate product-owned bounded chunks with explicit count,
   finite-value, cancellation, and output-budget checks.
3. Use reusable cylinder/sphere templates only when both radii are equal,
   clipping is disabled, and both cap modes are spherical. Taper, butt caps,
   hemispherical caps, or clipping require authored geometry or tessellation.

The simple fixture has 12 beams, clipping/taper semantics, and zero beams
eligible for the template shortcut. The feasibility tessellator used 12 radial
segments and emitted 4,320 triangles, below its fixed 262,144-triangle cap,
with a deterministic preview hash. It intentionally overdraws a full sphere
for `HemiSphere` and does not implement clipping. That output proves bounded
allocation and recognizable geometry only; 3MF-005 must implement exact
butt/hemisphere/clipping semantics, error selection, chunking, and authored
representation validation before the fallback is a product path.

## Closed error mapping

| Observed condition | Spike status | Product rule |
| --- | --- | --- |
| lib3mf error 10 | `Cancelled` | existing cancelled taxonomy |
| callback short read / identity change / seek failure | `IoFailure` | source changed or I/O failure, chosen from broker context |
| strict parse or other lib3mf exception | `Malformed` | refine only with product preflight/validated context |
| extraction count/allocation failure | `ResourceLimit` | existing limit taxonomy |
| unknown native exception | `InternalError` | generic internal importer failure |
| Job pressure observed as lib3mf generic error 5 | failure plus worker retirement | broker/Job evidence owns resource-limit classification |

Third-party exception strings remain worker-local. Only numeric library codes,
closed statuses, counters, and timings cross the private spike section. Product
warnings must similarly be translated to bounded product-owned warning IDs;
lib3mf text must not become default UI or clipboard content.

## Verification

- Debug and Release `Preview3D.slnx` builds completed successfully.
- Focused `[3mf-spike]` passed 6 cases / 721 assertions in Debug and Release.
- A complete Debug ImportIsolation run exercised every 3MF case successfully;
  its 18 failing cases were FBX fixture-rewrite and USD fixture/OpenUSD-host
  issues outside this change.
- PE inspection confirmed parser/ZIP DLLs are absent from the viewer and
  thumbnail provider.

## 3MF-002 handoff

3MF-002 may proceed. It must keep the callback identity/exact-read contract,
add independent OPC policy before lib3mf, deny Secure Content and unsupported
required extensions, map Job termination separately from lib3mf error 5, and
retire a worker after pressure. It should copy the private route's tests into
the eventual closed production opcode rather than exposing the spike mode.
The public `.3mf` extension and all thumbnail work remain deferred to their
planned tasks.
