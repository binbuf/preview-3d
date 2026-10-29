# Completes a self-update without keeping the viewer running during setup.
#
# The viewer starts this script non-elevated, then closes. The script waits for
# that process to exit, runs the verified installer with NSIS's /S silent
# switch (the UAC prompt is the only interaction), and reopens the app with the
# model path passed by the viewer. It is invoked by platform/UpdateApply.cpp.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$ParentProcessId,
    [Parameter(Mandatory = $true)][string]$InstallerPath,
    [Parameter(Mandatory = $true)][string]$ExpectedSha256,
    [Parameter(Mandatory = $true)][string]$AppPath,
    [string]$OpenPath = ''
)

$ErrorActionPreference = 'Stop'

function Show-UpdateError([string]$Message) {
    try {
        Add-Type -AssemblyName PresentationFramework
        [System.Windows.MessageBox]::Show(
            $Message,
            'Preview 3D update',
            'OK',
            'Error') | Out-Null
    } catch {
        # The update is already closed; the app can be started manually.
    }
}

try {
    # Wait for the viewer to release the executable and the model. Setup also
    # waits briefly for the window itself, which covers a slow shutdown.
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq (Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue)) {
            break
        }
        Start-Sleep -Milliseconds 250
    }
    Start-Sleep -Milliseconds 250

    # Re-check the digest immediately before elevation: the installer lives in
    # the per-user Updates folder, outside the elevated installer's control.
    $actual = (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash
    if ($actual -notmatch '^[0-9A-Fa-f]{64}$' -or
        $actual.ToLowerInvariant() -ne $ExpectedSha256.ToLowerInvariant()) {
        throw 'The downloaded installer no longer matches its published SHA-256 digest.'
    }

    $installer = Start-Process -FilePath $InstallerPath -ArgumentList '/S' `
        -Verb RunAs -Wait -PassThru
    if ($null -ne $installer -and $null -ne $installer.ExitCode -and
        $installer.ExitCode -ne 0) {
        throw "The installer exited with code $($installer.ExitCode)."
    }
    Remove-Item -LiteralPath $InstallerPath -Force -ErrorAction SilentlyContinue
} catch {
    Show-UpdateError "Preview 3D could not install the update.`n`n$($_.Exception.Message)"
    exit 1
}

try {
    # Windows file paths cannot contain a double quote, so wrapping the path
    # is sufficient for the app's single positional model argument.
    if (-not [string]::IsNullOrWhiteSpace($OpenPath) -and
        (Test-Path -LiteralPath $OpenPath -PathType Leaf)) {
        Start-Process -FilePath $AppPath -ArgumentList ('"' + $OpenPath + '"')
    } else {
        Start-Process -FilePath $AppPath
    }
} catch {
    Show-UpdateError "Preview 3D was updated but could not be reopened.`n`n$($_.Exception.Message)"
    exit 1
}
