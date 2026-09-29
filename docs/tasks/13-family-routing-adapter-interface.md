---
verify: x64\Release\Tests.Unit.exe
---
# T13 — Implement family routing and the adapter interface

## Goal
Wire the frozen adapter interface to the COM object so `GetThumbnail` dispatches to the correct
family adapter by CLSID, with no content sniffing, and maps adapter failures to the tabulated
HRESULTs.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — call contract, adapter routing and HRESULT table.
- `docs/tasks/04-freeze-provider-interfaces.md` — the adapter/sampler/raster contracts and routing table.
- `docs/tasks/12-bounded-stream-backing.md` — the bounded input an adapter receives.
- `docs/design/adr/0003-provider-decoder-scope.md` — which optional decoders an adapter may use.

## Scope
- [ ] Implement `IThumbnailProvider::GetThumbnail`: treat `cx` as a maximum physical-pixel hint, reject only `cx == 0`, clamp actual raster resolution independently.
- [ ] Select the adapter from the object's CLSID; never sniff another family or recover a path.
- [ ] Provide the adapter a bounded stream view, the deadline, and the provider limits; receive product-owned geometry/materials or a typed error.
- [ ] Map adapter outcomes to the tabulated HRESULTs, always setting a null bitmap on failure.
- [ ] Add a no-op/test adapter and tests proving routing per CLSID, `cx == 0` rejection, and each error mapping.

## Out of scope
- Real family parsing (→ T21–T34).
- Sampler and rasterizer implementation (→ T14–T15).

## Design notes
- Return a top-down 32-bit premultiplied BGRA DIB section and set `WTS_ALPHATYPE` to `WTSAT_ARGB`.
- Explorer owns the returned `HBITMAP`; release every other GDI object before returning.
- Never fabricate a success bitmap for a failed parse.

## Done when
- [ ] Routing and error-mapping tests pass in Debug and Release.
- [ ] `GetThumbnail` returns the exact HRESULT for each injected adapter failure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_