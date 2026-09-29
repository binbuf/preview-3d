# 0001 — The thumbnail provider supports eight format families, including STEP

## Status
accepted

## Context
FR-01 requires thirteen direct extensions across eight families: glTF, STL, PLY, OBJ, FBX, 3MF, USD and
STEP. The archived provider baseline (`05-thumbnail-provider.md` and ADR-011) says "seven CLSIDs" and
its table omits STEP, because STEP support was added to the product after that document was written.
STEP-009 requires a dedicated STEP thumbnail CLSID and `.step`/`.stp` registration. Shipping STEP in
the viewer but not in Explorer would leave a documented direct format without a thumbnail.

## Decision
The provider exposes **eight** fixed, stable CLSIDs — one per family, including STEP — and every
direct extension routes to its family CLSID. The seven existing identities in
`05-thumbnail-provider.md` are retained verbatim; STEP receives one new fixed identity, already
recorded in `overview.md` and `05-thumbnail-provider.md` and confirmed/frozen by T01 (it is not
regenerated). `.mtl` remains a sidecar and never gets a CLSID.

## Consequences
- `05-thumbnail-provider.md`, `08-installation-and-registration.md` and the risk register must say
  "eight", and the STEP row must be added to the CLSID table.
- Gate 6 and Gate 7 exit criteria apply to all eight CLSIDs, including the post-install check that
  each loads into the isolated surrogate.
- The STEP adapter carries the heaviest dependency (OCCT); ADR-0002 governs how it may be linked.