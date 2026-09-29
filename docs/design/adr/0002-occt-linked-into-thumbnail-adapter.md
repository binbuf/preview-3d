# 0002 — OCCT is linked only into a separately built STEP thumbnail adapter

## Status
accepted

## Context
`02-system-architecture.md` says OCCT is "AppContainer STEP host only … never the viewer, general
worker, or thumbnail provider", and that the STEP host and its OCCT payload are "not in the …
thumbnail closure". But `stp2.md` STEP-009 requires the STEP thumbnail to use a "separately built,
explicitly limited OCCT STEP/XDE/tessellation adapter … only into the isolated thumbnail DLL". The two
cannot both hold. The provider cannot launch `Preview3DStepHost.exe` (no process launch is permitted),
so a STEP thumbnail requires *some* in-DLL STEP path.

## Decision
Build a **separate constrained OCCT adapter** as its own static library target (a reduced module
closure pinned to the same OCCT version) and link it **only** into the thumbnail DLL, never into the
viewer, the general worker or either import host. The provider's STEP adapter consumes only
`IInitializeWithStream` through a bounded seekable stream and performs product-owned Part-21 admission
before any OCCT call, mirroring the STEP host's admission. The DLL still launches no process.

Because the provider runs under Shell surrogate isolation rather than the AppContainer boundary, the
OCCT adapter's safety rests on the provider's bounded reads, checked parsing, strict provider ceilings
and deadline/limit enforcement — not on the surrogate. This supersedes the "never the thumbnail
provider" wording in the archived `02-system-architecture.md`.

## Consequences
- The provider's dependency/licence/SBOM closure grows to include the constrained OCCT adapter (T42).
- The STEP adapter is the highest-risk family and is scheduled last (T34), gated on ADR-0001 and a
  feasibility check inside T34.
- T34 qualifies STEP against measured 384 MiB process-commit increase and 2 s time-to-return
  targets, including opaque OCCT calls. If it fails, T34 records the evidence and remains incomplete.
  A scope-changing ADR and roadmap/release-claim update are required before a release without STEP
  thumbnails can proceed; T41 must not register a knowingly non-rendering handler under the
  full eight-family claim. The contingency never weakens the target or launches a host.
