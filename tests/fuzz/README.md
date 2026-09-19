# No-GPU format fuzz targets

## STEP

`StepFuzz` is an explicitly built sanitizer target for the bounded static
STEP/STP admission boundary. It creates no window, GPU device, file mapping,
resolver, or child process, and it never links the pinned OCCT kernel. Its
bounded envelope selects one of four production surfaces:

- the product-owned `StepPart21Preflight` lexical admission scanner, including
  the `FILE_POPULATION`/`DOCUMENT_FILE` external-declaration discovery, in both
  its pure byte form and the handle-streaming form;
- declaration discovery with the external-document ceiling raised, so the
  scanner keeps lexing instead of failing closed at the first declaration;
- the production framed control decoder with mutated
  `StartStepImportFromFile` request-flag masks and `StepProgress` records; and
- the trusted normalized-output copy-and-validate decoder.

```powershell
python tests/fuzz/prepare_step_seeds.py TestResults/step-006/fuzz-seeds
msbuild tests/fuzz/StepFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/StepFuzz.exe TestResults/step-006/fuzz-seeds -max_total_time=45 -timeout=5 -rss_limit_mb=1024 -max_len=1048576 -print_final_stats=1 -verbosity=0
```

The 1 MiB standalone input cap is deliberately smaller than the product's
Tier-B primary-source cap, and the scanner limits are reduced so an in-process
run is bounded. The pinned OCCT transfer/tessellation containment stays in the
real AppContainer/Job ImportIsolation tests. The seed preparer refuses a
non-empty output directory because libFuzzer evolves the corpus it receives.

## 3MF

`ThreeMfFuzz` instruments the product-owned OPC/ZIP preflight, bounded Deflate
and CRC extraction, and the worker's XML display/lattice scan. It takes no
model path, sidecar, network, window, or GPU. The standalone envelope can also
force cancellation. The 1 MiB input, 4 MiB expansion, 128-entry, and 32:1
limits keep an in-process fuzz run bounded; the production AppContainer and Job
limits are separately exercised by ImportIsolation.

```powershell
python tests/fixtures/3mf/verify.py
python tests/fuzz/prepare_3mf_seeds.py TestResults/3mf-007/fuzz-seeds-fresh
msbuild tests/fuzz/ThreeMfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/ThreeMfFuzz.exe TestResults/3mf-007/fuzz-seeds-fresh -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=1048576 -print_final_stats=1 -verbosity=0
```

The seed preparer refuses a non-empty directory because libFuzzer evolves the
corpus it receives. `lib3mf` itself is a separately built app-local DLL and is
not sanitizer-instrumented by this target; model construction, full adapter
normalization, process containment, and recovery remain real-worker tests.

## USD

`UsdFuzz` is an explicitly built multi-domain sanitizer target for USD-009. It
creates no window, GPU device, filesystem-derived resolver, or child process.
Its bounded envelope selects one of five production surfaces:

- TinyUSDZ USDA object-graph parsing and a bounded normalization walk, plus
  USDC byte classification (mutated USDC parsing stays in the Job-contained
  real-worker regression);
- product-owned USDZ central/local-directory, path, expansion, CRC, and
  cancellation preflight;
- trusted normalized-output copy-and-validate;
- the compatibility host's exact pure identifier/anchoring policy for
  constrained virtual dependency maps; and
- the production framed control decoder with mutated OpenUSD start and
  resolver-sidecar records.

```powershell
python tests/fixtures/usd/verify.py
python tests/fuzz/prepare_usd_seeds.py TestResults/usd-009/fuzz-seeds
msbuild tests/fuzz/UsdFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/UsdFuzz.exe TestResults/usd-009/fuzz-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0
```

The 2 MiB standalone input cap is deliberately smaller than the product's
Tier-B cap. OpenUSD itself is a separately built pinned DLL and is not
sanitizer-instrumented by this target; its resolver, composition, process, Job,
and recovery boundary remains covered by real-host ImportIsolation tests and
the hostile-process lane. TinyUSDZ has no enforceable allocator hook, so
arbitrary mutated USDC parsing likewise stays in the real worker behind its Job
limit. The seed preparer refuses non-empty output directories because
libFuzzer mutates its corpus in place. Any sanitizer finding must be minimized
into the immutable USD corpus before release signoff.

## FBX

`FbxFuzz` is an explicitly built sanitizer target; it is not part of the
shipping solution. It fuzzes pinned ufbx load options, progress cancellation,
a one-entry bounded virtual sidecar stream, deterministic start-pose
evaluation, a bounded normalized scene walk, triangulation, and the trusted
host's protocol/shared-section copy-and-validate decoder. It creates no window
or GPU device and never opens a model-derived path.

```powershell
python tests/fuzz/prepare_fbx_seeds.py TestResults/fbx-007/fuzz-seeds
msbuild tests/fuzz/FbxFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
x64/Release/FbxFuzz.exe TestResults/fbx-007/fuzz-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=512 -max_len=1048576
```

Run the same command after adding any minimized fault to the immutable corpus.
The target's 16 MiB primary / 1 MiB virtual-sidecar caps and 64 MiB ufbx
allocator cap are intentionally lower than product limits so a smoke run has a
tight, deterministic envelope. Release qualification still relies on the real
AppContainer/Job tests for process containment and product-size caps.
