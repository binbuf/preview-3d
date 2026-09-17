# FBX qualification corpus

`manifest.json` freezes the redistributable ufbx 0.23.0 inputs already held in
`../fbx-spike`, their decoded payload hashes, provenance, independent product
expectations, and deterministic hostile/malformed derivations. Binary payloads
remain base64 in source control for reviewability; hashes cover decoded bytes.

Verify without writing files:

```powershell
python tests/fixtures/fbx/verify.py
```

Materialize decoded valid inputs and derived failures outside source control:

```powershell
python tests/fixtures/fbx/verify.py --output TestResults/fbx-007/corpus
```

The focused `[fbx]` ImportIsolation tests own normalized numeric goldens and
typed outcomes. The manifest does not derive expected values by asking ufbx;
that would make the parser its own oracle. Mutation recipes are intentionally
small and reviewable, and their output hashes are immutable.
