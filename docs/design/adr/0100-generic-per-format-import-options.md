# ADR-0100 — Generic per-format import options

**Status:** Accepted

## Context

T05 added the CAD `read_wire` seam, but it was a dormant policy field. The
STEP host already had Draft/Balanced/Full quality bits, and the USD host had no
way to select a subdivision level. Users could not reach any of these controls,
and a cache key could not distinguish two imports of the same file under
different quality or unit assumptions. F3D exposes a closed `FORMAT.name`
reader-option surface; the product needs the same shape without adopting F3D's
runtime or a free-form reader-option bag.

## Decision

One closed, validated `model_core::ImportOptions` value is shared by the
viewer, broker, worker and both hosts. It carries:

- `quality` (Draft/Balanced/Full) for every CAD format;
- tri-state `read_wire` (absent = T05 fallback, on, off);
- optional `linear_deflection`, `angular_deflection` and
  `relative_deflection` with bounded ranges;
- optional `unit_scale` (metres per transferred unit), applied only after the
  format's authored-unit resolution and recorded as a compatibility diagnostic;
- `usd_subdivision_level` (0..4) for the OpenUSD host.

The viewer and CLI use the product-owned grammar
`--import-option <FORMAT>.<name>=<value>`, case-insensitive in the format and
name. The broker validates the record before launching a process and packs it
into a fixed 48-byte `ImportOptionsPayload` appended to the STEP, generalized
CAD, USD fast-worker and OpenUSD host requests. `version == 0` keeps every
pre-T06 zero-filled request valid: the existing STEP quality flag bits remain
authoritative for quality and every other field keeps its documented default.
The worker and host re-validate the packed form. Unknown versions, unknown
mask bits, non-canonical values, out-of-range enums/numbers and inapplicable
fields are typed `ImportProtocolViolation` before any parser call.

`ImportOptionsCacheIdentity(format, options)` hashes the format and every
explicit/absent option value. `StepImporterCacheIdentity` mixes it with the
versioned STEP profile; `CacheEntryKey` carries the source format and that
identity alongside the existing source and importer identities. No path,
generation id or user text enters the identity.

This build links no subdivision evaluator, so a nonzero
`USD.subdivision_level` is a typed `UnsupportedRequiredFeature` from the
OpenUSD host (and the fast worker) rather than a silent control-cage render.
The option is still validated, routed and cache-keyed, so a later evaluator can
implement it without changing the grammar.

## Consequences

- Every output-changing option invalidates a derived-cache entry.
- The legacy `ImportSessionRequest::stepImportQuality` and STEP flag bits keep
  working; a generic record wins when supplied.
- Request structs grew by 48 bytes; all producers/consumers move together and
  the protocol tests pin the new sizes.
- The UI exposes a labelled Import options menu with reset-to-defaults; the
  CLI and Settings JSON remain per-format for scripts.
- Arbitrary reader-supplied strings, third-party parser internals, animation,
  cameras and lighting controls remain out of scope.
