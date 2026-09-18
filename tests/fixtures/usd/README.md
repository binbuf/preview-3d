# USD qualification corpus

`manifest.json` freezes the redistributable USDA, USDC, and USDZ inputs held in
`../usd-spike`, their decoded-byte hashes, provenance, independent product
expectations, and deterministic malformed/unsafe/limit derivations. Base64 is
only a source-control transport for the two upstream binary fixtures; hashes
and byte counts cover the decoded payloads consumed by the importers.

Verify the immutable sources and every derivation without writing files:

```powershell
python tests/fixtures/usd/verify.py
```

Materialize decoded and derived inputs outside source control for manual,
AppContainer, or fuzz qualification:

```powershell
python tests/fixtures/usd/verify.py --output TestResults/usd-009/corpus
```

The focused `[usd-004]`, `[usd-005]`, and `[usd-007]` ImportIsolation tests are
the numeric oracle for normalized geometry, hierarchy, instances, transforms,
materials, texture pixels, warnings, composition outcomes, recovery, and exact
fast/compatibility overlap. This manifest deliberately records expectations
independently instead of asking TinyUSDZ or OpenUSD to generate its own oracle.
The deterministic derivations keep hostile inputs reviewable and reproducible.
