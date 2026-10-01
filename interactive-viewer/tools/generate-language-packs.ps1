#requires -Version 7
<#
.SYNOPSIS
  Regenerates the viewer language packs under interactive-viewer/lang from the
  English fallback literals embedded in the source.

.DESCRIPTION
  Localization.h documents the model: every user-facing string is written in
  place as `Loc("key", L"English")`. This tool scans the source for those
  (key, English) pairs, writes lang/en.json as the translator catalogue, and
  produces lang/<code>.json for every other language by machine translation.

  Keys that appear more than once with different English text are reported and
  only the first seen is kept — a collision means two call sites disagree and
  must be fixed before shipping.

  The machine translations are a starting point for community review, not a
  substitute for it. Existing hand-edited packs are overwritten only with
  -Force.

.PARAMETER Languages
  Pack codes to generate. English ("en") is always regenerated.

.PARAMETER SkipTranslate
  Only rebuild en.json (no network access).
#>
[CmdletBinding()]
param(
    [string]$SourceDir = (Join-Path $PSScriptRoot '..\src'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\lang'),
    [string[]]$Languages = @(
        'ar', 'az', 'bn', 'ckb', 'cs', 'de', 'el', 'en-GB', 'es', 'fa', 'fi', 'fr',
        'he', 'hi', 'hr', 'hu', 'id', 'it', 'ja', 'ko', 'lt', 'nb', 'nl', 'pl',
        'pt', 'pt-BR', 'ro', 'ru', 'sk', 'sv', 'ta', 'th', 'tr', 'uk', 'vi',
        'zh-Hant', 'zh_Hans'),
    [switch]$SkipTranslate,
    [switch]$Force,
    # Verify that the committed packs match the source and are well-formed,
    # without writing anything or touching the network. Exits non-zero on any
    # drift, key collision, missing/extra key, empty value, or placeholder
    # mismatch. This is the language-pack CI gate.
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# Some Google Translate locales differ from our pack codes.
$apiLocale = @{
    'en-GB'   = 'en-GB'
    'pt'      = 'pt-PT'
    'nb'      = 'no'
    'he'      = 'iw'
    'zh_Hans' = 'zh-CN'
    'zh-Hant' = 'zh-TW'
    'ckb'     = 'ckb'
}

function Get-CppString([string]$literal) {
    # Decode the C++ escape sequences the source literals may contain.
    try { return [regex]::Unescape($literal) } catch { return $literal }
}

function ConvertTo-JsonString([string]$value) {
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    foreach ($ch in $value.ToCharArray()) {
        switch ([int]$ch) {
            0x22 { [void]$builder.Append('\"'); continue }
            0x5C { [void]$builder.Append('\\'); continue }
            0x08 { [void]$builder.Append('\b'); continue }
            0x0C { [void]$builder.Append('\f'); continue }
            0x0A { [void]$builder.Append('\n'); continue }
            0x0D { [void]$builder.Append('\r'); continue }
            0x09 { [void]$builder.Append('\t'); continue }
            default {
                if ([int]$ch -lt 0x20) { [void]$builder.AppendFormat('\u{0:x4}', [int]$ch) }
                else { [void]$builder.Append($ch) }
            }
        }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Test-Placeholders([string]$english, [string]$translated) {
    $expected = ([regex]::Matches($english, '\{[0-9]\}') | ForEach-Object { $_.Value } | Sort-Object) -join ','
    $actual = ([regex]::Matches($translated, '\{[0-9]\}') | ForEach-Object { $_.Value } | Sort-Object) -join ','
    return $expected -eq $actual
}

function Read-PackMap([string]$path) {
    return [System.IO.File]::ReadAllText($path, $utf8) | ConvertFrom-Json -AsHashtable
}

# --- Collect (key, English) pairs -----------------------------------------
# Keys are shaped like "area.name" (camelCase after the first dot). Requiring a
# dot keeps the scan from matching unrelated (narrow string, wide string)
# neighbours, and the lowercase first character matches the naming convention.
$pairPattern = '"([a-z][A-Za-z0-9._-]*\.[A-Za-z0-9._-]+)"\s*,\s*L"((?:[^"\\]|\\.)*)"'
$pairs = [System.Collections.Specialized.OrderedDictionary]::new()
$conflicts = @()
Get-ChildItem -Path $SourceDir -Recurse -File -Include '*.cpp', '*.h' | ForEach-Object {
    $text = [System.IO.File]::ReadAllText($_.FullName, $utf8)
    foreach ($m in [regex]::Matches($text, $pairPattern)) {
        $key = $m.Groups[1].Value
        $english = Get-CppString $m.Groups[2].Value
        if ($pairs.Contains($key)) {
            if ($pairs[$key] -ne $english) {
                $conflicts += "  $key : '$($pairs[$key])'  vs  '$english'"
            }
        } else {
            $pairs[$key] = $english
        }
    }
}

Write-Host "Collected $($pairs.Count) strings."
if ($conflicts.Count -gt 0) {
    Write-Warning "Key collisions found (fix these — each key must map to one string):"
    $conflicts | ForEach-Object { Write-Warning $_ }
}

# --- Verify mode (CI gate) ------------------------------------------------
# Source is canonical: check the committed catalogue and every language pack
# against the scanned pairs. Writes nothing and makes no network calls.
if ($Verify) {
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($collision in $conflicts) { $problems.Add("key collision:$collision") }

    $enPath = Join-Path $OutputDir 'en.json'
    $catalogue = $null
    if (-not (Test-Path -LiteralPath $enPath -PathType Leaf)) {
        $problems.Add("missing catalogue: $enPath (run the generator)")
    } else {
        try { $catalogue = Read-PackMap $enPath }
        catch { $problems.Add("en.json is not valid JSON: $($_.Exception.Message)") }
    }
    if ($null -ne $catalogue) {
        foreach ($key in $pairs.Keys) {
            if (-not $catalogue.ContainsKey($key)) { $problems.Add("en.json missing key: $key") }
            elseif ([string]$catalogue[$key] -ne [string]$pairs[$key]) { $problems.Add("en.json value drift: $key (regenerate)") }
        }
        foreach ($key in $catalogue.Keys) {
            if (-not $pairs.Contains($key)) { $problems.Add("en.json stale key not in source: $key") }
        }
    }

    $expectedCodes = @('en') + $Languages
    foreach ($code in $expectedCodes) {
        $path = Join-Path $OutputDir "$code.json"
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $problems.Add("missing language pack: $code.json")
            continue
        }
        $pack = $null
        try { $pack = Read-PackMap $path }
        catch { $problems.Add("$code.json is not valid JSON: $($_.Exception.Message)"); continue }

        foreach ($key in $pairs.Keys) {
            if (-not $pack.ContainsKey($key)) {
                $problems.Add("$code.json missing key: $key")
                continue
            }
            $value = [string]$pack[$key]
            if ([string]::IsNullOrEmpty($value)) { $problems.Add("$code.json empty value: $key") }
            elseif (-not (Test-Placeholders ([string]$pairs[$key]) $value)) {
                $problems.Add("$code.json placeholder mismatch: $key")
            }
        }
        foreach ($key in $pack.Keys) {
            if (-not $pairs.Contains($key)) { $problems.Add("$code.json extra key: $key") }
        }
    }

    if ($problems.Count -gt 0) {
        Write-Host ""
        Write-Host "Localization verification FAILED: $($problems.Count) problem(s)." -ForegroundColor Red
        $problems | Select-Object -First 80 | ForEach-Object { Write-Host "  $_" }
        if ($problems.Count -gt 80) { Write-Host "  ... and $($problems.Count - 80) more" }
        exit 1
    }
    Write-Host "Localization verification passed: $($pairs.Count) keys across $($expectedCodes.Count) packs."
    exit 0
}

# --- Write en.json --------------------------------------------------------
if (-not (Test-Path -LiteralPath $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }

function Write-Pack([string]$code, [System.Collections.IDictionary]$map) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('{')
    $index = 0
    foreach ($key in $map.Keys) {
        $index++
        $comma = if ($index -lt $map.Count) { ',' } else { '' }
        $lines.Add('  ' + (ConvertTo-JsonString $key) + ': ' + (ConvertTo-JsonString ([string]$map[$key])) + $comma)
    }
    $lines.Add('}')
    [System.IO.File]::WriteAllText((Join-Path $OutputDir "$code.json"), ($lines -join "`n") + "`n", $utf8)
}

Write-Pack 'en' $pairs
Write-Host "Wrote en.json"

if ($SkipTranslate) { return }

# --- Machine-translate the rest ------------------------------------------
function Protect([string]$value) {
    # Keep placeholders and hard line breaks intact through the translator.
    $value = $value -replace "`n", '@@NL@@'
    $value = [regex]::Replace($value, '\{(\d)\}', '@@$1@@')
    return $value
}
function Restore([string]$value) {
    $value = $value -replace '@@NL@@', "`n"
    # Tolerate a translator dropping one '@' from a protected token.
    $value = [regex]::Replace($value, '@@?(\d)@@?', '{$1}')
    return $value
}

function Invoke-Translation([string[]]$texts, [string]$target) {
    $query = ($texts | ForEach-Object { 'q=' + [uri]::EscapeDataString($_) }) -join '&'
    $uri = "https://translate.googleapis.com/translate_a/t?client=gtx&sl=en&tl=$([uri]::EscapeDataString($target))&$query"
    for ($attempt = 0; $attempt -lt 4; $attempt++) {
        try {
            $result = Invoke-RestMethod -Uri $uri -TimeoutSec 60
            if ($result -is [string]) { return @($result) }
            return @($result)
        } catch {
            Start-Sleep -Seconds (1 + $attempt * 2)
        }
    }
    return $null
}

$keys = @($pairs.Keys)
foreach ($code in $Languages) {
    if ($code -eq 'en') { continue }
    $targetPath = Join-Path $OutputDir "$code.json"
    if ((Test-Path -LiteralPath $targetPath) -and -not $Force) {
        Write-Host "Skipping $code (exists; use -Force to overwrite)"
        continue
    }
    $target = if ($apiLocale.ContainsKey($code)) { $apiLocale[$code] } else { $code }

    $translated = [ordered]@{}
    $batch = New-Object System.Collections.Generic.List[string]
    $batchKeys = New-Object System.Collections.Generic.List[string]
    $batchLength = 0
    $failed = $false

    foreach ($key in $keys) {
        $english = [string]$pairs[$key]
        $batch.Add($english)
        $batchKeys.Add($key)
        $batchLength += $english.Length + 4
        # Flush a full-enough batch, and always flush the last one.
        if ($batchLength -lt 1200 -and $key -ne $keys[$keys.Count - 1]) { continue }

        $protected = @($batch | ForEach-Object { Protect $_ })
        $response = Invoke-Translation $protected $target
        if (-not $response -or $response.Count -ne $batch.Count) {
            # Fall back to English for this batch rather than dropping keys.
            for ($i = 0; $i -lt $batchKeys.Count; $i++) { $translated[$batchKeys[$i]] = $pairs[$batchKeys[$i]] }
            $failed = $true
        } else {
            for ($i = 0; $i -lt $batchKeys.Count; $i++) {
                $translated[$batchKeys[$i]] = Restore ([string]$response[$i])
            }
        }
        $batch.Clear(); $batchKeys.Clear(); $batchLength = 0
        Start-Sleep -Milliseconds 120
    }

    # Preserve source order, repairing any string whose placeholders did not
# survive translation (a dropped {0} would render a sentence with a missing
# value) back to English.
    $ordered = [ordered]@{}
    foreach ($key in $keys) {
        $value = [string]$translated[$key]
        if ([string]::IsNullOrEmpty($value) -or -not (Test-Placeholders ([string]$pairs[$key]) $value)) {
            $value = [string]$pairs[$key]
        }
        $ordered[$key] = $value
    }
    Write-Pack $code $ordered
    $note = if ($failed) { ' (some batches kept English)' } else { '' }
    Write-Host "Wrote $code.json$note"
}