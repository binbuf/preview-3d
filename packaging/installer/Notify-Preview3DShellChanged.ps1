# T41 non-elevated Shell association-change notification.
#
# The installer invokes this from a basic-user (restricted) token in the
# initiating interactive session so a machine-level registration change is
# announced without requiring elevation (design/08, ADR-0028):
#
#   SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, null, null)
#
# It changes no state and is non-fatal; registration remains correct if the
# notification is delayed until Explorer next refreshes. The installer also
# broadcasts from its own process as a guaranteed fallback.

[CmdletBinding()]
param()

$ErrorActionPreference = 'SilentlyContinue'
Set-StrictMode -Version 3.0

try {
    if (-not ('Preview3DShellNotify' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Preview3DShellNotify {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(uint eventId, uint flags, IntPtr item1, IntPtr item2);
}
'@
    }
    [Preview3DShellNotify]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
} catch {
    # Notification is best-effort and must never fail setup or uninstall.
}

exit 0