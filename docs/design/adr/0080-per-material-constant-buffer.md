# ADR-0080 - Per-material constant buffer and per-slot texture binding

- **Status:** accepted
- **Date:** 2026-10-30
- **Task:** T22

## Context

T22 must apply `normalTexture.scale`, five independent texture slots (including
the new occlusion map) and per-slot UV set/transform/sampler metadata. The
textured root signature was already at D3D12's 64-DWORD limit: six tables plus
25 inline draw DWORDs plus the frame CBV leave no room for either a sixth SRV
table or the per-slot material data. An earlier attempt to add the sixth table
and a `b3` CBV without freeing budget hung every textured load.

## Decision

1. The whole material record moves from 32 root-constant DWORDs (`b2`) to a
   per-material `b2` constant buffer. The CBV costs 2 DWORDs instead of 32; the
   root signature becomes `b0` CBV (2) + six single-descriptor SRV tables (6) +
   `b1` draw constants (25) + `b2` material CBV (2) = 35 of 64 DWORDs.
2. Texture registers are `t0` baseColor, `t1` metallicRoughness, `t2` normal,
   `t3` emissive, `t4` studio environment cubemap and `t5` occlusion. Root
   parameters 1-6 are those tables in order; `b1` is parameter 7 and the
   material CBV is parameter 8.
3. The C++ record is 304 bytes of `float4` rows: the v12 lobe block, the legacy
   UV block, `gMaterialExtras` (`normalScale`, mirrored sign), five per-slot
   UV transforms and five packed sampler words. Its CBV stride is 512 bytes so
   every binding address is 256-byte aligned.
4. A buffer is shared by every draw with the same material chunk and mirrored
   sign, because the mirrored sign is the only per-draw value in the record.
   The buffer carries that ownership tag; an instance cloned from shared
   geometry calls `EnsureMaterialConstants` when its effective material or
   handedness differs, so two draws can never overwrite one another's record.
   Each buffer holds `kFrameCount` 512-byte regions; `RenderFrame` writes the
   current frame's region immediately before binding it. UPLOAD-heap memory is
   persistently mapped, needs no transition, and the frame slot has already
   retired behind its fence, so no material write races a sampled frame.
5. The packed sampler word is `addressU | addressV << 2 | nearest << 4 |
   uvSet << 5`, with bit 9 marking an authored UV set this build cannot sample.
   A slot whose word marks an unsupported set is skipped, never aliased to UV0.

## Consequences

- T25/T26/T27 material additions extend this record and its stride, not the
  inline root-constant budget. A new record size is a protocol/wire change.
- Material memory is one 1.5 KiB buffer (three 512-byte regions) per material
  and mirrored-sign pair, included in the viewer's allocation accounting and
  upload estimate.
- The material record can no longer be varied per draw inside one material
  without splitting the buffer key; per-instance values must travel through
  `b1` draw constants instead.
- The T21/T22 diagnostics record is the contract for what these fields contain;
  `tests/render-quality/diagnostics.py` must move with any field addition.
