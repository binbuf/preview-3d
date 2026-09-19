# 3MF-007 qualification record

Status: in progress (2026-09-18)

Baseline: `88c04bc` (3MF-006)

Scope: viewer only; Explorer thumbnails remain 3MF-008

## Completed in this slice

- Froze a SHA-256 manifest for seven decoded source packages and seven
  deterministic derived cases. Six source packages come from the pinned lib3mf
  2.5.0 test corpus under its recorded license. The seventh is a product-authored
  Production repackage with optional Slice data. The verifier checks decoded
  lengths, hashes, and the exact repackage relation without regenerating goldens.
- Added malformed ZIP/OPC, traversal, unknown-required-extension, and private
  metadata cases. A real AppContainer regression asserts typed rejection and a
  later valid open after each archive failure. The Bambu-style metadata case is
  product-authored; it is not claimed as a versioned Bambu conformance sample.
- Added an isolated ASan/libFuzzer target over production OPC preflight, bounded
  Deflate/CRC extraction, cancellation, and the worker XML display/lattice scan.
  It does not instrument the separately built lib3mf DLL or the full adapter.
- Fixed overflow-prone ZIP64 ratio arithmetic. The corpus uncovered that the
  worker accepted an unknown required extension. The XML scan now rejects
  unknown required namespace URIs before lib3mf load. It also rejects the
  upstream Production sample because that sample requires unsupported Slice;
  the frozen optional-Slice derivative preserves its standard two-item build.

## Commands and local results

Run from repository root. MSBuild is the Visual Studio 18 Community binary on
this machine; a Developer PowerShell may use `msbuild` instead.

```powershell
python tests/fixtures/3mf/verify.py
python tests/fuzz/prepare_3mf_seeds.py TestResults/3mf-007/fuzz-seeds-verified
msbuild tests/fuzz/ThreeMfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/ThreeMfFuzz.exe TestResults/3mf-007/fuzz-seeds-verified -max_total_time=10 -timeout=5 -rss_limit_mb=1024 -max_len=1048576 -print_final_stats=1 -verbosity=0
msbuild Preview3D.slnx /t:Tests_ImportIsolation,Tests_Unit /p:Configuration=Debug /p:Platform=x64 /m
msbuild Preview3D.slnx /t:Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64 /m
x64/Debug/Tests.ImportIsolation.exe "[3mf-003],[3mf-004],[3mf-005],[3mf-006],[3mf-007]" --reporter compact
x64/Release/Tests.ImportIsolation.exe "[3mf-003],[3mf-004],[3mf-005],[3mf-006],[3mf-007]" --reporter compact
x64/Release/Tests.ImportIsolation.exe "[3mf-spike],[hostile-worker]" --reporter compact
python tests/app-smoke/three_mf.py --configuration Release
```

- Corpus verifier: 7 sources and 7 derived cases matched hashes.
- Final Release sanitizer smoke: 145,971 executions in 11 seconds from 14
  fresh seeds; no crash, sanitizer finding, or timeout; peak reported RSS
  440 MiB. An earlier 30-second run before the final root-node scan cleanup
  also completed without a finding; only the 11-second run is final-binary
  evidence.
- Final Debug and Release focused real-worker tests: 11 cases / 1,757
  assertions each. The first final Debug rebuild hit `C1041` because Visual
  Studio was concurrently compiling the same worker PDB. After that IDE build
  finished, a serial rebuild and focused rerun both passed; the test command
  that followed the failed build is excluded from the evidence.
- Release 3MF spike plus protocol hostile-worker lane: 19 cases / 1,058
  assertions, including Job/cancellation recovery and the new optional-Slice
  fixture. The upstream required-Slice source remains a spike-only API sample.
- Debug Unit: 99 cases / 7,629 assertions.
- Release real-app smoke: 12 activation/viewer/recovery checks pass, including
  two-item Production rendering from the optional-Slice fixture. Output was
  saved under ignored `TestResults/3mf-007/app-smoke-release.json`.
- The optional local manual-corpus test passed 17 assertions against the three
  untracked slicer files in `test-models/`; they are not redistributable or part
  of the frozen manifest.

The attempted broad Release `[sandbox],[sidecar-resolver],[hostile-worker]`
selector was not green: 33 of 36 cases passed, with failures in OpenUSD host and
USD fixture paths. A serial Release Unit rerun also failed 16 GPU device/queue
initializations after the app smoke, while the same Debug Unit suite passed.
Neither run is release evidence. No 3MF route failed in the focused runs; these
broader failures still need investigation before signoff.

## Remaining release gates

3MF-007 and Gate 4 remain open. The frozen corpus still needs applicable
official Consortium conformance samples, independently licensed representative
Prusa/Orca/Bambu files, more ZIP64/streaming/relationship/texture/lattice
adversarial cases, and numeric render goldens. The current `InspectThreeMfOpc`
checks ZIP structure and part names, but does not yet parse and validate
`[Content_Types].xml` or OPC relationship targets as promised by
`.docs/3mf.md` and the design. That policy gap needs code and real-worker
regressions before release. The upstream `beam-lattice.3mf.base64` production
failure also needs classification against its authored feature requirements.

Still unrun: full Debug/Release isolation and graphics/security sweeps on a
stable test host; median/p95 performance, commit/cancellation/UI telemetry for
all 3MF workload classes; dependency vulnerability/static-analysis closure;
8-hour mixed-open soak; clean offline standard-user install/repair/upgrade/
uninstall and portable lifecycle; signed candidate, installed hashes, and SBOM
inspection. No signed Release artifact or clean VM was available for this slice.
