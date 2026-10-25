#Requires -Version 5.1
<#
.SYNOPSIS
Runs one bounded libFuzzer smoke target (seed preparation plus the fuzz run).

.DESCRIPTION
This is the body of the `fuzz-smoke` CI matrix step, extracted so the same
smoke can be run locally or from a manual dispatch without copying the
workflow's argument list. The target executable must already be built; the CI
workflow builds it immediately before calling this script.

The script throws on a nonzero exit from either the seed-preparation script or
the fuzz binary, which is what makes the CI step fail.

.PARAMETER Target
Matrix label used in error messages (for example STEP or Provider).

.PARAMETER Executable
Path to the built libFuzzer executable.

.PARAMETER PrepareScript
Python seed-preparation script for this target.

.PARAMETER Seeds
Seed corpus directory passed to the preparation script and the fuzzer.

.PARAMETER Timeout
Per-input timeout in seconds. ProviderFuzz uses 10 because OCCT tessellation on
a mutated STEP seed can exceed the default 5.

.PARAMETER MaxTotalTime
Total fuzzing time in seconds. CI deliberately keeps this short (60).

.PARAMETER RssLimitMb
libFuzzer RSS limit in MiB.

.PARAMETER MaxLen
Maximum input length in bytes.

.EXAMPLE
./tests/fuzz/Invoke-FuzzSmoke.ps1 -Target STEP `
  -Executable tests/fuzz/x64/Release/StepFuzz.exe `
  -PrepareScript tests/fuzz/prepare_step_seeds.py `
  -Seeds TestResults/step-008/fuzz-seeds
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Target,

    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,

    [Parameter(Mandatory = $true)]
    [string]$Seeds,

    [ValidateRange(1, 600)]
    [int]$Timeout = 5,

    [ValidateRange(1, 86400)]
    [int]$MaxTotalTime = 60,

    [ValidateRange(64, 1048576)]
    [int]$RssLimitMb = 1024,

    [ValidateRange(1, 1073741824)]
    [int]$MaxLen = 1048576
)

$ErrorActionPreference = 'Stop'

foreach ($path in @($Executable, $PrepareScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "$Target input '$path' was not found."
    }
}

Write-Host "Preparing $Target seeds in '$Seeds'"
& python $PrepareScript $Seeds
if ($LASTEXITCODE -ne 0) {
    throw "$Target seed preparation failed with exit code $LASTEXITCODE."
}

Write-Host "Running $Target fuzz smoke for $MaxTotalTime s (timeout ${Timeout}s, RSS ${RssLimitMb} MiB)"
& $Executable $Seeds `
    "-max_total_time=$MaxTotalTime" `
    "-timeout=$Timeout" `
    "-rss_limit_mb=$RssLimitMb" `
    "-max_len=$MaxLen" `
    -print_final_stats=1 `
    -verbosity=0
if ($LASTEXITCODE -ne 0) {
    throw "$Target fuzz smoke failed with exit code $LASTEXITCODE."
}
