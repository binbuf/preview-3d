# 0096 — STEP interchange aliases stay in the existing catalog family

Status: accepted (T02, 0.8.0).

## Context

STEP interchange files are commonly named `.step`, `.stp`, `.stpnc`, `.p21`, or
`.210`; the F3D reference declares and documents the same five suffixes. The
0.8.0 catalog owned only `.step` and `.stp`, so a valid Part-21 file with one of
the other names failed the automatic open gate with `UnsupportedFormat` even
though the STEP host accepts the bytes. The installer and the thumbnail
provider's registration script each carried an independent extension list, so
adding a suffix in one place could silently leave the others behind.

## Decision

- `model_core::kStepExtensions` owns `step`, `stp`, `stpnc`, `p21`, and `210`.
  The aliases share `FormatId::Step`, the `step` stable name,
  `Binbuf.Preview3D.STEP.1`, the STEP thumbnail CLSID, and the existing host.
  They add no format, family, CLSID, parser, or process launch path.
- The catalog remains the identity authority. The NSIS install/uninstall lists,
  `thumbnail-provider/FamilyRouting.h`, and the staged
  `Preview3DThumbnailRegistration.ps1` table must agree with it. The
  `[format-aliases]` unit cases assert the catalog lookup, the direct-open list,
  the Open As text, the NSIS macros, the provider route, and the registration
  script; the existing `[format-installer]` case still covers the full catalog.
- A suffix is added only when the shipped adapter validates the content. The
  STEP host still performs its Part-21 admission, so an alias never bypasses
  byte validation. No suffix is guessed from third-party tools.
- The valid-file scoreboard entries for the three aliases are copies of the
  repository-generated AP214 fixture and must open through the STEP host, not
  remain an extension-class failure.

## Consequences

- The viewer, Open dialog, command line, Open With ranking, installer, and
  Explorer thumbnail registration all accept the five STEP suffixes
  case-insensitively. Docs and the README list the same set.
- A later alias or format change must update the catalog and its drift-checked
  mirrors in the same change; the `[format-aliases]` cases fail otherwise.
- The provider roster stays frozen at eight families (ADR-0001); aliases extend
  an existing family and are not a provider-expansion precedent.
