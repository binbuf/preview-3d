# FBX-007: corpus, hardening, and release qualification

Status: ready — FBX-006 complete
Depends on: FBX-001 through FBX-006  
Unblocks: viewer FBX support claim and FBX-008

## Objective

Produce the evidence required by Gate 4 for the FBX viewer slice. This task is
not a documentation-only signoff: missing corpus, fuzz, security, limit,
performance, or package evidence must be implemented and rerun.

## Context to load

- `design/03-file-formats-and-ingestion.md` — verification and Tier-B limits
- `design/09-quality-performance-and-security.md`
- `design/10-delivery-plan.md` — Gate 4
- `design/11-decisions-and-risks.md` — R-02/R-13/R-17 and spike 5
- `.docs/TODO.md`, `.docs/PROGRESS.md`
- `tests/fixtures/{README.md,generate.py,verify.py,manifests}`
- `tests/performance/qualify.py`
- `tests/hostile-worker` and all ImportIsolation FBX tests
- packaging/SBOM/signature scripts and current OBJ qualification gaps

## Work

1. Check in a redistributable, provenance-recorded FBX corpus with immutable
   hashes and independent expectations:
   - matching binary/ASCII static scenes;
   - hierarchy/instances/transforms/units/axes and large coordinates;
   - skin, blend, combined deformation, multiple stacks/non-zero start;
   - PBR factors, embedded textures, approved/missing/unsafe sidecars;
   - unsupported optional and required feature examples;
   - empty, malformed, truncated, adversarial counts/depth, NaN/Inf, allocation
     pressure, and a representative bounded B-each performance scene.
2. Add the standalone no-GPU FBX fuzz target with a constrained virtual
   sidecar map and seeds for load options, callbacks, deformation, normalized
   output, and malformed protocol records. Run short sanitizer fuzz smoke and
   record commands/results; minimize every discovered fault into the corpus.
3. Rerun protocol and hostile-worker attacks against the FBX route, including
   post-copy mutation, stale generations, scene graph cycles/references, invalid
   layout/transform data, progressive replays, and sidecar request abuse.
4. Measure median/p95 import time and peak worker private commit for the B-each
   fixture. Verify Tier-B source/count/scratch/texture caps, cancellation gates,
   UI heartbeat, worker timeout/Job termination, and valid reopen. Record
   hardware/build/cache conditions; do not claim Tier-A multi-gigabyte targets.
5. Verify direct path/dialog/drop/secondary activation and clean offline
   standard-user install/Open With/uninstall. Audit the portable and NSIS
   closure, `ufbx` license, SBOM component/version/hash, signatures when a
   candidate certificate is available, and absence of unreviewed dependencies.
6. Review cache-version impact. If persistent derived writes exist when this
   task runs, include FBX parser/evaluator/options and protocol schema in the
   cache key and prove old entries miss. If cache remains deferred, record that
   there is no FBX cache entry to invalidate and reserve the importer version
   tuple; do not fabricate cache evidence.
7. Update `.docs/TODO.md`, `.docs/PROGRESS.md`, format/support documentation,
   and a new `.docs/fbx/FBX-007-VERIFICATION.md` with exact commands, case
   counts, results, hashes, known limitations, and unverified environmental
   gates. Mark Gate 4 Slice 2 complete only if every required item passes.

## Exit criteria

- Expected hierarchy, instances, transforms, pose, bounds, materials, textures,
  counts, and warnings match independent goldens for binary and ASCII FBX.
- Malformed/over-limit/unsupported input fails safely and the next open works.
- Parser/decoder code remains confined to the AppContainer worker; unbrokered
  file, network, plug-in, codec, and child-process access remains impossible.
- Full Debug and Release builds, Unit, ImportIsolation, hostile-worker,
  app-smoke, fixture verification, package closure, and fuzz smoke pass.
- Tier-B performance/memory/cancellation results are recorded and meet the
  applicable design gates, or the design is revised rather than waived.
- Documentation accurately distinguishes viewer FBX support from the still
  separate Explorer-thumbnail deliverable.
