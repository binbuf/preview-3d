# Explorer thumbnail provider

## Scope

thumbnail-provider builds an in-process x64 COM DLL used by Windows Explorer to render model thumbnails. It shares validation, normalized math, material fallbacks, and bounded parsers with model-core, but it does not share the interactive renderer, open a D3D12 device, launch the viewer, start worker processes, or make network requests.

The provider implements:

- IInitializeWithStream to receive the Shell-managed content stream;
- IThumbnailProvider to produce a requested-size HBITMAP;
- IClassFactory plus DllGetClassObject and DllCanUnloadNow;
- explicit module/object/lock reference counts.

The DLL has no registration side effects in DllMain. DllMain only records the module handle and disables unnecessary thread notifications.

## COM classes and extension assignment

Distinct CLSIDs let the factory select an adapter without sniffing every grammar. The values below are product identities and must not be regenerated.

| Family | Extensions | CLSID |
| --- | --- | --- |
| glTF | .glb, .gltf | {A592F425-EA68-4C88-BB96-020805D4BE56} |
| STL | .stl | {BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9} |
| PLY | .ply | {F4DC6119-E235-4BAC-8089-54EDD84F8492} |
| OBJ | .obj | {D4722752-C480-4D9C-BEBE-1A9B514A8846} |
| FBX | .fbx | {FBC218D4-FD2C-41DF-B168-7F3B9E53C84E} |
| 3MF | .3mf | {D8389A63-8526-454A-9892-72F3149484B9} |
| USD | .usd, .usda, .usdc, .usdz | {E938BC70-4C08-4446-A15D-EE31576BFB48} |

Each class is registered as an InprocServer32 with ThreadingModel=Apartment and attached to the extension's ShellEx thumbnail-handler GUID. The MSI writes these machine-level registrations; users remain in control of default open applications.

Registering a handler as InprocServer32 is necessary for Explorer to load it, but it is not what isolates it: by default the Shell loads thumbnail handlers into an isolated per-handler COM surrogate (normally `DllHost.exe`) rather than into `explorer.exe` itself, and that surrogate process boundary — not apartment threading, not the COM contract — is what contains a parser crash inside this DLL away from Explorer. (`Prevhost.exe` is a different, unrelated surrogate that Windows uses to host `IPreviewHandler` for the Preview pane; this product implements no `IPreviewHandler` and is never hosted by it — see [01-product-scope.md](./01-product-scope.md).) The installer, its registry entries, and any troubleshooting documentation MUST NOT set `DisableProcessIsolation=1` (or an equivalent per-handler opt-out) for any of the seven CLSIDs above, in the MSI or in support guidance. A future change that enables in-process (`explorer.exe`-hosted) execution for performance reasons requires a new ADR, a revised threat model in [09-quality-performance-and-security.md](./09-quality-performance-and-security.md), and re-justifying every claim in this document that currently depends on Shell process isolation.

## Call contract

Initialize:

- accepts exactly one non-null IStream;
- takes an independent stream reference and rejects a second initialization;
- queries STATSTG when supported, but does not trust its size without checked reads;
- seeks only if the stream advertises it; adapters that need random access copy into the bounded backing store;
- never assumes a filesystem path or attempts to recover one.

GetThumbnail:

1. Treats `cx` as the caller's requested maximum physical-pixel dimension per [`IThumbnailProvider::GetThumbnail`](https://learn.microsoft.com/windows/win32/api/thumbcache/nf-thumbcache-ithumbnailprovider-getthumbnail) and rejects only the degenerate `cx == 0`; it never fails a call merely because `cx` exceeds today's common Explorer cache sizes, since the Shell's cached thumbnail sizes are documented as subject to change. The actual raster resolution is independently clamped regardless of `cx` (see CPU renderer below), so a larger future request costs no extra correctness risk.
2. Establishes a monotonic deadline: 750 ms target and 2 seconds hard internal cutoff.
3. Parses metadata and samples geometry within the thumbnail budgets in [03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md).
4. Frames finite bounds, renders a deterministic CPU image, and returns a top-down 32-bit premultiplied BGRA DIB section.
5. Sets WTS_ALPHATYPE to WTSAT_ARGB.

Explorer owns the returned HBITMAP. The provider releases every other GDI object before returning. A failed call sets the output bitmap to null and returns a precise HRESULT.

## Stream ingestion

256 MiB is the maximum source data this provider will ever read or cache from the stream; it is not a product-wide maximum file size. A multi-gigabyte GLB or STL still opens normally in the full viewer ([03-file-formats-and-ingestion.md](./03-file-formats-and-ingestion.md)) — it simply receives no Explorer thumbnail, and Explorer falls back to the generic file icon. When STATSTG reports a size over 256 MiB, the provider fails fast to that fallback rather than attempting a partial read, so an oversized file costs Explorer no more than a quick size check. The provider does not write a temporary or persistent file. Seek-capable streams under the budget are accessed through serialized, bounded range reads and a small block cache. If an adapter requires one contiguous buffer, the provider may create a checked in-process backing buffer up to 128 MiB; a larger input on that path returns the safe generic-icon fallback. Non-seekable inputs may use that same bounded backing buffer. Reads abort on deadline, limit, or short-read inconsistency.

There is no MapViewOfFile zero-copy guarantee for Shell IStream inputs. This is intentional: Shell isolation, bounded memory, and deterministic latency matter more than sharing the interactive viewer's source mapping implementation.

External dependencies are unavailable in the provider:

- .obj renders geometry without its MTL or texture sidecars;
- .gltf renders only when required geometry buffers are embedded as data URIs within limits; bounded Draco geometry and KTX2/WebP textures are allowed when their stricter provider decode budgets and deadline hold. A missing optional image uses the default material, while unavailable required geometry safely falls back to the generic icon;
- .ply and .stl are fully stream-contained and may use their bounded mesh/point sampling paths;
- .fbx may use bounded embedded geometry/textures;
- .3mf and .usdz may use contained package entries;
- .usd/.usda/.usdc do not resolve external references.

The full viewer retains the broader local-sidecar support defined in the format document.

## Geometry sampling

The provider must not normalize a multi-million-triangle model in full merely to draw a 256-pixel image. Each adapter enumerates triangles into a deterministic spatial/reservoir sampler:

- bounds are accumulated from finite vertices;
- at most 2 million input triangles or 6 million input points are inspected and at most 250,000 representative triangles/points enter rasterization;
- material boundaries and disconnected large components receive minimum representation;
- degenerate/non-finite triangles are discarded;
- a stable source-derived seed ensures Explorer cache consistency.

If trustworthy format metadata supplies bounds, it can guide sampling but is verified against sampled positions. For formats that cannot stream geometry safely under the limits, the provider stops and lets Explorer show its generic icon.

## CPU renderer

The thumbnail DLL uses a product-owned tile rasterizer; no GPU device or graphics queue is created inside Explorer.

- Render at min(max(cx, 64), 512), with 2x supersampling only when the deadline budget permits.
- Use a transparent canvas, soft neutral floor/contact shadow, and the same neutral material palette as the viewer.
- Frame a fixed isometric view from verified bounds with 7% margin.
- Apply model transforms in double precision, clip against the near plane, depth-test tiles, and shade with ambient plus two fixed lights.
- Render opaque/masked triangles. Approximate transparent materials as weighted opaque color; exact order-independent transparency is unnecessary at thumbnail size.
- Render point-cloud samples as depth-tested round splats with deterministic size and source/neutral color.
- Downsample in linear space and convert to premultiplied BGRA.

No text, file path, watermark, network content, or nondeterministic animation appears in the bitmap.

## Threading and unload

An object is apartment-affine. GetThumbnail performs work on the calling thread because the Shell owns call scheduling; it does not create a lasting pool. Parsing libraries are invoked with per-call arenas and no process-global mutable caches. A deadline check is included at bounded parser/sampler/raster tiles.

DllCanUnloadNow returns S_OK only when live objects, class-factory locks, and active calls are all zero. Destructors are noexcept and release stream/backing resources. Thread-local parser scratch cannot keep the module artificially alive.

## Security and robustness

The DLL is treated as hostile-input code executing in a sensitive host:

- compile with /guard:cf, /CETCOMPAT, /DYNAMICBASE, /NXCOMPAT, /sdl, and high warning level;
- use checked integer/range helpers at every file-derived allocation or offset;
- disable parser callbacks that open paths, URLs, plug-ins, scripts, codecs, or environment-selected resources;
- never start or communicate with the OpenUSD compatibility host, the general import worker, or the viewer, and never read the viewer's persistent derived cache;
- rely on Shell's default out-of-process surrogate hosting as the actual crash-containment boundary (see COM classes and extension assignment above); the compile-time hardening flags below reduce what a crash can do, they do not replace process isolation;
- treat that surrogate hosting as crash containment for Explorer only, not as the zero-capability AppContainer security boundary that [ADR-014](./11-decisions-and-risks.md#adr-014-appcontainer-import-processes-are-the-parser-security-boundary-not-threads) requires of the import worker: the surrogate still runs with the invoking user's own token and ordinary file-system/network access, so this DLL's actual safety against a hostile file comes from the bounded reads, checked parsing, and deadline/limit enforcement in this document, not from the process boundary;
- place third-party parser calls behind exception and structured-exception containment at the COM boundary where legally safe, while fixing ordinary memory faults rather than masking them;
- write no model-derived persistent cache;
- keep diagnostic events path-redacted and disabled unless troubleshooting is enabled.

An importer crash must be addressed by fuzzing/fixing; SEH containment is a last-resort HRESULT boundary, not a correctness mechanism.

## HRESULT mapping

| Condition | HRESULT |
| --- | --- |
| Bad pointer/invalid call order | E_POINTER / E_UNEXPECTED |
| Unsupported stream behavior or format feature | HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) |
| Malformed or empty geometry | HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) |
| Limit or deadline exceeded | HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) / ERROR_TIMEOUT |
| Allocation failure | E_OUTOFMEMORY |
| Decoder/importer failure | E_FAIL, with diagnostic event |

Explorer is allowed to fall back to the generic icon. Returning a fabricated “success” bitmap for a failed parse would poison the Shell thumbnail cache and is prohibited.

## Tests

- COM identity, QueryInterface, aggregation rejection, refcount, lock server, and unload tests.
- One extension-routing test per registry entry and CLSID.
- PLY mesh/point-cloud golden and hostile-list tests; glTF Draco/KTX2 provider-limit tests.
- Golden images at 32, 64, 256, and 512 pixels with tolerant perceptual comparison.
- STA parallel-host stress using multiple COM objects.
- Truncation, archive bomb, adversarial count, non-seekable stream, timeout, OOM injection, and fuzz corpora.
- Repeated Explorer surrogate load/unload with GDI/User handle and private-byte leak checks.
- Verification in the actual Windows thumbnail surrogate at 100%, 150%, and 200% DPI.
- Post-install verification, on a clean machine, that each registered CLSID is actually loaded into the isolated surrogate process (not `explorer.exe`) and that no installed registry value sets `DisableProcessIsolation`; this is a release-blocking check, not an optional audit.

Primary references: [Thumbnail provider guidance](https://learn.microsoft.com/windows/win32/shell/thumbnail-providers), [IInitializeWithStream](https://learn.microsoft.com/windows/win32/api/propsys/nn-propsys-iinitializewithstream), and [IThumbnailProvider::GetThumbnail](https://learn.microsoft.com/windows/win32/api/thumbcache/nf-thumbcache-ithumbnailprovider-getthumbnail).
