# Windows download and protection guidance

Preview 3D is distributed through this repository’s [GitHub Releases](https://github.com/binbuf/preview-3d/releases). Until code-signing and reputation work is complete, Windows may warn about a new or unsigned build. A warning does not establish that a file is unsafe, but it does mean you should verify it before running it.

## Verify the download first

Only download from the project’s Releases page. Each release includes a matching `.sha256` file. In PowerShell, calculate the download’s SHA-256 and compare it with that file’s value:

```powershell
Get-FileHash .\Preview3D-<version>-x64-setup.exe -Algorithm SHA256
```

Do not run a build whose source or checksum you cannot verify. On a work or school device, follow your organization’s policy or contact its administrator.

## Choose the warning that matches what you see

### “This file came from another computer…”

This is the Windows Attachment Manager mark applied to downloaded files. After verifying the source and checksum:

1. In File Explorer, right-click the downloaded ZIP or installer and select **Properties**.
2. On the **General** tab, select **Unblock** in the Security section.
3. Select **Apply**, then **OK**.

For a ZIP, unblock the ZIP *before* extracting it so its contents do not inherit the mark. Microsoft’s [Attachment Manager guidance](https://support.microsoft.com/en-us/windows/security/information-about-the-attachment-manager-in-microsoft-windows) describes this checkbox and its security implications.

### “Windows protected your PC”

This is commonly a Microsoft Defender SmartScreen reputation warning. Confirm that the file came from the Releases page and that its hash matches. If Windows presents **More info** followed by **Run anyway**, use it only after those checks. Do not disable antivirus or edit security policies merely to run the app.

### “Smart App Control blocked this app”

Smart App Control (SAC) is different: it does **not** offer a per-app or per-file exception, so the Properties **Unblock** checkbox and a SmartScreen override will not bypass SAC. Microsoft explains that SAC allows apps it recognizes as safe or that have a valid signature; otherwise it can block them.

If you have verified this release and still choose to run it, the available user-level option is to turn off SAC:

1. Open **Windows Security**.
2. Select **App & browser control**.
3. Open **Smart App Control settings**.
4. Set **Smart App Control** to **Off**.

Turning off SAC lowers protection for all apps, not just Preview 3D. Do this only for a release you trust, and do not change it on a managed device without approval. On current Windows 11 releases, Microsoft says SAC can be re-enabled from Windows Security when it is available on the device.

## What we are doing

The project is working toward properly code-signed release artifacts and the reputation required by Windows protection services. In the meantime, releases provide SHA-256 checksums so you can independently verify the exact file you downloaded.

For the technical details, see Microsoft’s [Smart App Control FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions) and the [related Microsoft Q&A discussion](https://learn.microsoft.com/en-us/answers/questions/5637638/smart-app-blocked-my-app).
