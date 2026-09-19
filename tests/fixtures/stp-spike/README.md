# STEP-001 spike harness and fixtures

Test-only sources for the [STEP-001 spike](../STEP-001-SPIKE-RESULTS.md). None
of this is linked into product code, and `Preview3D.exe`,
`Preview3DImportWorker.exe`, and the thumbnail provider never see it.

The fixtures are generated, not downloaded, by OCCT's XDE writer so the corpus
is reproducible without network access once the constrained OCCT port is
restored. The product host never links the writer; only
`GenerateStepFixtures.cpp` does.

| File | Purpose |
| --- | --- |
| `GenerateStepFixtures.cpp` | Writes the five AP203/AP214/AP242 fixtures with the XDE writer. |
| `StepSpikeLocalDriver.cpp` | Runs `RunStepSpike` in-process against a fixture for measurement. |
| `StepSpikeSandboxHarness.cpp` | Launches the real `Preview3DStepSpike.exe` through the product AppContainer/Job launcher and asserts success, preflight rejection, cancellation, Job termination/recovery, and authority denial. |
| `StepAuthorityProbe.cpp` | Separate executable launched under the same container to prove path, network, and child-process denial. |

## Build (Release, against the constrained OCCT install)

```text
cl /nologo /std:c++20 /EHsc /MD /O2 /DNDEBUG /W4 /WX /permissive- /utf-8 ^
   /I<occt>\include\opencascade /I..\..\compatibility-host-step\src ^
   /I..\..\shared\platform\include ^
   <repo>\compatibility-host-step\src\main.cpp ^
   <repo>\compatibility-host-step\src\StepXdeSpike.cpp ^
   <repo>\shared\platform\src\MappedView.cpp ^
   /Fe:Preview3DStepSpike.exe /link /LIBPATH:<occt>\lib TK*.lib
```

`StepSpikeSandboxHarness.cpp` additionally links
`shared/import-broker/src/SandboxLauncher.cpp`,
`shared/platform/src/ProcThreadAttributeList.cpp`,
`shared/platform/src/AppContainerSid.cpp`,
`shared/import-broker/src/SharedSection.cpp`, and
`shared/platform/src/MappedView.cpp`, and needs `userenv.lib advapi32.lib`.

The spike host and its OCCT DLLs must be co-located in one payload directory;
the harness grants that directory read+execute to the AppContainer SID and
deliberately does **not** grant the fixture directory, proving handle-only
input.
