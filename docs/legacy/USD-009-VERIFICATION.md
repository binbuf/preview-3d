# USD-009 qualification record

Status: in progress — immutable corpus and standalone fuzz-smoke lane complete
Date: 2026-09-18
Baseline: `c1b43af` (USD-008)

## Completed in this slice

The checked-in USD qualification manifest covers 10 redistributable source
fixtures and 13 deterministic derived cases. It freezes decoded-byte sizes and
SHA-256 values, provenance, and independent expectations for USDA/USDC/USDZ,
`.usd` byte-sniffing inputs, hierarchy, transforms, all three axes, units and
large origins, mesh primvars, point instances, material subsets, Preview
Surface, the complete supported composition set, malformed input, unsafe
archive/reference paths, recursion, unsupported subdivision, invalid metadata,
and dependency pressure. The existing real-worker/host `[usd-004]`,
`[usd-005]`, and `[usd-007]` tests remain the numeric semantic oracle and exact
fast/compatibility comparison; the manifest never derives expectations from
either parser.

`UsdFuzz` is a standalone no-window/no-GPU libFuzzer target. Its bounded
envelope drives five domains: TinyUSDZ USDA object graphs plus render-data
normalization (and the USDC byte classifier), product USDZ preflight, trusted
normalized-output validation, the compatibility resolver's pure identifier
policy, and production control framing seeded with OpenUSD-start and resolver
requests.
The pinned OpenUSD DLL is not sanitizer-instrumented; real-host composition,
resolver, Job, fault, and recovery testing therefore remains in
ImportIsolation rather than being misrepresented as an ASan lane.

## Commands and results

Run from the repository root in a Visual Studio Developer PowerShell:

```powershell
python tests/fixtures/usd/verify.py
python tests/fuzz/prepare_usd_seeds.py TestResults/usd-009/fuzz-seeds
msbuild tests/fuzz/UsdFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/UsdFuzz.exe TestResults/usd-009/fuzz-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0

msbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Debug /p:Platform=x64 /m
x64/Debug/Tests.Unit.exe --reporter compact
x64/Debug/Tests.ImportIsolation.exe "[usd-002],[usd-003],[usd-004],[usd-005],[usd-006],[usd-007],[usd-008],[usd-009]" --reporter compact
x64/Debug/Tests.ImportIsolation.exe "[sandbox],[sidecar-resolver]" --reporter compact

msbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Release /p:Platform=x64 /m
x64/Release/Tests.Unit.exe --reporter compact
x64/Release/Tests.ImportIsolation.exe "[usd-002],[usd-003],[usd-004],[usd-005],[usd-006],[usd-007],[usd-008],[usd-009]" --reporter compact
x64/Release/Tests.ImportIsolation.exe "[sandbox],[sidecar-resolver]" --reporter compact
```

Results on the local Windows 11 x64 development machine:

- corpus verification: 10 source fixtures and 13 derived cases, all hashes
  matched;
- fuzz seed materialization: 11 seeds spanning all five domains;
- clean-seed Release ASan/libFuzzer smoke: 38,814 executions in 61 seconds, no
  crash, sanitizer finding, or timeout; peak reported RSS 492 MiB under the
  1,024 MiB cap;
- Debug and Release solution target builds: pass;
- Unit: 98 cases / 7,625 Debug assertions and 98 cases / 7,537 Release
  assertions (the three established Release debug-layer availability warnings
  remain);
- focused real-process USD-002 through USD-009: 28 cases / 756 assertions in
  each configuration. Debug's USD-002 composition sample reported 14.095 ms
  startup, 24.552 ms `LoadNone`, 149.047 ms first geometry, and 50,749,440-byte
  peak working set. Release reported 3.784 ms, 3.198 ms, 10.730 ms, and
  26,947,584 bytes respectively. These single diagnostic samples are not
  median/p95 performance qualification.
- focused AppContainer, Job, filesystem/network/process-spawn denial, and
  sidecar containment selector: 22 cases / 301 assertions in each
  configuration. Its first Release run exposed a stale test scratch-directory
  collision; after replacing process-ID/stack-address names with a Windows
  allocated unique temp name, both configurations pass.

The first sanitizer link exposed an ABI detail worth retaining: Visual C++
ASan enables STL string/vector/optional annotations, while the ordinary pinned
TinyUSDZ static library was compiled without them. The target disables those
three container annotations to match the library ABI; ASan still instruments
the harness and product-owned USDZ/control/output-boundary sources. A fully
instrumented TinyUSDZ requires a separate sanitizer triplet/build, not a forced
link across mismatched STL ABIs.

The target must keep its project-private `tests/fuzz/x64` output. An early
draft redirected it to the solution's shared `x64/Release` directory;
`msbuild /t:Rebuild` then treated sibling app-local DLLs as foreign outputs and
removed `fastgltf.dll` and `simdjson.dll`, causing every Release worker route
to report `WorkerCrashed` before parsing. Rebuilding the worker closure and
isolating the fuzz output fixed the run. Standalone targets must never clean or
own the product's shared solution output directory.

The seed preparer now refuses a non-empty destination. libFuzzer evolves the
directory it receives, and a mistakenly reused 216-file working corpus reached
the RSS oracle in the long-lived sanitizer process. A new 11-seed directory
completed within the cap. Arbitrary mutated USDC is intentionally not parsed
in-process: a minimized crate mutation can cause a multi-gigabyte TinyUSDZ
allocation before its advisory memory option reacts. That input is frozen as
`allocation-pressure.usdc` and exercised only through the real AppContainer
worker with a 128 MiB Job cap; the test accepts controlled rejection or Job
termination/replacement and then proves a valid USDC import succeeds.

## USD hostile-input security review

| Risk | USD code path and consequence | Defensive control and bounded evidence |
|---|---|---|
| Memory safety / malformed input | TinyUSDZ, USDZ preflight, control frames, and normalized shared output consume attacker-controlled bytes; unchecked lengths could corrupt or confuse the trusted viewer. | ASan fuzzes the product boundaries with a 2 MiB input cap and 5-second unit timeout. The immutable truncated/non-finite/metadata corpus and real-process tests require typed failure and recovery. The viewer copies and revalidates shared output through `ValidateAndCopySection`. |
| Resource exhaustion | TinyUSDZ's USDC memory setting is advisory and the minimized crate mutation can request excessive allocation. | Both import processes run in kill-on-close Jobs. The USD-009 128 MiB Job regression contains or replaces the worker and proves the next valid import succeeds; dependency, archive, depth, byte, and request caps remain enforced. |
| Path traversal / external resources | OpenUSD composition and texture asset paths could otherwise escape the model directory or select a URI/drive/UNC path. | `OpenUsdIdentifier.h` is now the single production/fuzzed virtual-identifier policy. It rejects authority changes, `..`, absolute, drive, UNC, backslash, and colon-bearing references. Only the trusted broker canonicalizes and opens allowed local sidecars by handle. |
| Network access | An authored remote asset or resolver plug-in could disclose data or retrieve active content. | `http`/URI identifiers fail the resolver policy, both parser processes are zero-capability AppContainers, and OpenUSD environment/plugin discovery is locked to the hash-audited private manifest. Existing sandbox tests verify network denial. |
| Unsafe deserialization / IPC | A compromised parser could forge frame sizes, generations, offsets, counts, or mutable-section contents. | Closed-version framed messages, bounded payloads, generation checks, and copy-then-revalidate semantics are fuzzed/tested before viewer use. No native pointer or open-ended object graph crosses the boundary. |
| Command execution / privilege boundary | A parser compromise could attempt child-process execution or leverage ambient host privilege. | The worker and compatibility host are `asInvoker`, zero-capability AppContainers assigned to Jobs before untrusted processing; they inherit only explicit handles and cannot spawn children. The viewer loads no format parser or OpenUSD plug-in. No USD code path invokes a shell or command interpreter. |
| Temporary files | Parser-controlled temp names/content could create races or executable persistence. | USD parsing and dependency service are memory/handle based and create no parser-selected temporary file. Test-only corpus materialization stays under ignored `TestResults`; product temporary-store policy remains random-name, user-only ACL, delete-on-close data. |

No security/content filter prevented a test in this USD-009 session. One generic
shell deletion was rejected by the execution safety layer; the three exact,
locally generated fuzz artifacts were subsequently removed with validated
workspace-local paths. The unrun gates below are environmental/release gates,
not filter omissions.

## Cache and dependency record

No format adapter writes normalized geometry to the viewer's persistent cache;
`DerivedCache` is still an unwired Gate 2 opaque-payload prototype. There is
therefore no USD cache entry to invalidate or backend-equivalence claim to
make. Reserve this importer identity tuple before any USD cache integration:

`usd-static-policy=1; wire=10; tinyusdz=0.9.1#2@a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb; openusd=26.8.0@ee47c679abde5b467a7b6a41f3b2285564a4222e`

The tuple must also change for any normalization-policy, texture-decoder, or
schema/resource-manifest behavior change. Both backends must canonicalize to
the same keyed payload before persistent writes are enabled.

## Remaining USD-009 gates

USD-009 and Gate 4 are not complete. The focused hostile-worker lane passes in
both configurations at 13 cases / 273 assertions, and the two new USD-009
regressions pass 45 assertions in each configuration. Still required are the full
Debug/Release ImportIsolation reruns with unrelated baseline failures either
fixed or explicitly separated, repeated median/p95 worker and host
performance/commit measurements, UI/Present heartbeat evidence, package and
tamper reruns against the final candidate, clean offline standard-user
install/repair/upgrade/rollback/uninstall and loaded-host replacement on a
fresh VM, an 8-hour soak, and Authenticode verification when a candidate
certificate exists. Explorer thumbnails remain exclusively USD-010.
