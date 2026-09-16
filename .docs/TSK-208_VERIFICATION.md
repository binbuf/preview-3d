# TSK-208 verification (2026-09-15)

TSK-208 completes the Tier A vertex and material contract and makes point-only
PLY displayable content. Protocol v7 adds the 64-byte
`PositionNormalUv0TangentColor_F32` layout while retaining the existing
position-only and 32-byte layouts. glTF, Draco and PLY imports preserve bounded
normal, UV, tangent and color data. Missing triangle normals are generated in
the worker; tangents are generated only for a normal-mapped primitive that has
UVs but no authored tangent. STL keeps its neutral material behavior. Node
occurrences retain their node/mesh identities and double origins.

The D3D12 point path uses `PointList` directly. A geometry shader expands each
point on the GPU into a depth-tested round quad whose radius is camera-scaled
and clamped to 2–12 pixels. Position-only points receive a neutral color and the
complete layout consumes authored PLY color. No CPU point-to-triangle expansion
or per-point index buffer is created.

The complete mesh shader consumes vertex color, base color, metallic/roughness,
normal and emissive textures and factors, `KHR_materials_unlit`, the normalized
UV transform, alpha mask/cutoff and output alpha. Opaque/masked and blended
draws have distinct depth-write/blend states; double-sided materials select the
no-cull variants. Opaque draws group by compatible material identity. Blended
draws follow them in far-to-near bounds-center depth order. Feature-level-11
direct draws remain the correctness path.

Every material draw binds four valid descriptors. Missing slots use immutable
white base/MR, neutral normal and black emissive textures. Progressive batches
rebuild one model-wide shader-visible catalog and retain displaced descriptor
heaps through the last direct fence. This is required when geometry, material
and image dependencies arrive in different publications. Neutral resources,
descriptor heaps, pending uploads and retirement remain part of TSK-207's GPU
accounting; per-fine admission charges shared neutral resources once.

## Repeatable commands

Run the GPU/app checks sequentially on an interactive Windows desktop:

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
foreach ($configuration in 'Debug', 'Release') {
    & $taskMsbuild Preview3D.slnx /m /p:Configuration=$configuration /p:Platform=x64
    & "./x64/$configuration/Tests.Unit.exe" --reporter compact
    & "./x64/$configuration/Tests.ImportIsolation.exe" --reporter compact
    python tests/app-smoke/progressive.py --configuration $configuration --output "TestResults/tsk208-$configuration-progressive.json"
    python tests/app-smoke/run.py --configuration $configuration --runs 1 --require-points --output "TestResults/tsk208-$configuration-points.json"
    python tests/app-smoke/textures.py --configuration $configuration --output "TestResults/tsk208-$configuration-textures.json"
}
```

`FixtureManifestTests` checks frozen counts/bounds and reads complete-layout
color bytes: STL is neutral while the qualification GLB and both mesh/point PLY
fixtures retain color. glTF and PLY adapter tests check exact layout, normals,
UVs and colors. Wire tests pin the new stride and protocol. Existing frozen
PNG/JPEG/Basis decode-to-product-upload tests read every GPU mip back and compare
its pixels. The progressive app check forces geometry, material and image into
separate batches, verifies the textured draw survives the catalog rebuild, and
also covers cancellation and bounded queues. The lifecycle check requires the
point PLY to reach Ready and remain navigable through resize/reopen. Debug runs
compile every shader at runtime and report D3D12 debug-layer errors.

## Local results

- Debug and Release solution builds: succeeded, 0 warnings and 0 errors.
- Unit: 85/85 cases; 7,318 Debug and 7,230 Release assertions.
- ImportIsolation: 193/193 cases and 51,826 assertions in each configuration.
- Progressive: all four modes pass in both configurations. The forced run
  presents 11 of 64 chunks before completion, reaches all 64, and preserves one
  textured draw when its dependency catalog crosses batch boundaries.
- Point-required lifecycle: zero failures in Debug and Release across GLB,
  STL, big-endian PLY mesh, little-endian PLY points and glTF sidecar reopen.
- Texture runtime: low/full mip publication and corrupt-image fallback pass in
  both configurations. Debug reports an available debug layer and zero errors.

Frozen local reports are under `tests/fixtures/baselines/tsk-208/`.

## Qualification limits

The texture suite's pixel readback proves decoded/uploaded image bytes; the
app checks prove that the production material and point pipelines compile,
bind, present and remain debug-layer clean. This task does not add a separate
offline reference-rasterizer oracle for every PBR term. Final repeated visual
qualification and reference-system Draw-heavy frame targets remain TSK-302 and
TSK-305. Indirect submission, meshoptimizer LOD construction and WebP are still
deferred. No dependency, license, persistent-cache or worker path-authority
change is introduced.
