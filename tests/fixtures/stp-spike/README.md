# STEP-001 spike harness and STEP-003 fixtures

Test-only sources and the immutable STEP/STP fixtures used by the
[STEP-001 spike](../STEP-001-SPIKE-RESULTS.md) and
[STEP-003 scene adapter](../STEP-003-VERIFICATION.md). None of the harness
sources is linked into product code, and `Preview3D.exe`,
`Preview3DImportWorker.exe`, and the thumbnail provider never see them.

## Fixtures

The `.stp` files are the frozen STEP-003 test inputs. They were generated with
OCCT's XDE writer by `GenerateStepFixtures.cpp`; the product host never links
the writer. They are committed as clear-text ISO 10303-21 files and the tests
open them by path through the broker, which duplicates an already-open
read-only handle into the zero-capability host. The writer embeds a creation
timestamp, so regenerating does not reproduce the committed bytes exactly;
the committed files are the durable artifact.

| File | Coverage |
| --- | --- |
| `part_ap203.stp` | AP203 `CONFIG_CONTROL_DESIGN`, analytic solid with through-hole, shape color, millimetre |
| `part_ap214.stp` | AP214 `AUTOMOTIVE_DESIGN`, same geometry, shape color |
| `part_ap242.stp` | AP242 `AP242_MANAGED_MODEL_BASED_3D_ENGINEERING_MIM_LF` |
| `assembly_ap214.stp` | Shared box definition reused twice, cylinder, translated occurrence, transparent color |
| `assembly_nested_ap214.stp` | Nested sub-assembly reused twice over shared box/cylinder definitions |
| `instance_color_ap214.stp` | One definition placed three times; middle instance color overrides the definition color |
| `face_color_ap214.stp` | Per-face subshape colors with no shape-level color (material seam split) |
| `inch_part_ap214.stp` | `CONVERSION_BASED_UNIT('INCH')` authored length unit |

Sizes and SHA-256 hashes are recorded in
[`../../.docs/STEP-003-VERIFICATION.md`](../../.docs/STEP-003-VERIFICATION.md).

## Harness sources

| File | Purpose |
| --- | --- |
| `GenerateStepFixtures.cpp` | Writes the AP203/AP214/AP242 parts and assembly/instance/face-color fixtures with the XDE writer. |
| `StepSpikeLocalDriver.cpp` | Runs `RunStepSpike` in-process against a fixture for measurement. |
| `StepSpikeSandboxHarness.cpp` | Launches the real `Preview3DStepSpike.exe` through the product AppContainer/Job launcher and asserts success, preflight rejection, cancellation, Job termination/recovery, and authority denial. |
| `StepAuthorityProbe.cpp` | Separate executable launched under the same container to prove path, network, and child-process denial. |

## Build (Release, against the constrained OCCT install)

Generate the fixtures (the generator needs `TKMesh`/`TKXMesh` and the writer
in addition to the core list below), then build the harness:

```text
call vcvars64.bat
cl /nologo /std:c++20 /EHsc /MD /O2 /DNDEBUG /W4 /WX /permissive- /utf-8 ^
   /I<occt>\include\opencascade ^
   GenerateStepFixtures.cpp ^
   /Fe:GenerateStepFixtures.exe /link /LIBPATH:<occt>\lib TK*.lib
```

The spike host is the same recipe documented in STEP-001 for
`Preview3DStepSpike.exe`. `StepSpikeSandboxHarness.cpp` additionally links
`shared/import-broker/src/SandboxLauncher.cpp`,
`shared/platform/src/ProcThreadAttributeList.cpp`,
`shared/platform/src/AppContainerSid.cpp`,
`shared/import-broker/src/SharedSection.cpp`, and
`shared/platform/src/MappedView.cpp`, and needs `userenv.lib advapi32.lib`.

The spike host and its OCCT DLLs must be co-located in one payload directory;
the harness grants that directory read+execute to the AppContainer SID and
deliberately does **not** grant the fixture directory, proving handle-only
input.