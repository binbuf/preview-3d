$ErrorActionPreference = 'Stop'
$msvc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$sdkDir = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdk = $sdkDir.FullName
$sdkVer = $sdkDir.Name

$toolsDir = $PSScriptRoot
$repoRoot = Split-Path -Parent $PSScriptRoot           # interactive-viewer/
$gitRoot = Split-Path -Parent $repoRoot                # repo root -- vcpkg_installed/ lives here
$vcpkgInstalled = "$gitRoot\vcpkg_installed\x64-windows\x64-windows"
$objDir = "$repoRoot\obj_test"
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

& "$msvc\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++20 /W4 /utf-8 /D_UNICODE /DUNICODE `
    "/I$msvc\include" "/I$sdk\um" "/I$sdk\shared" "/I$sdk\ucrt" "/I$repoRoot\src\app" `
    "/I$vcpkgInstalled\include" `
    "$toolsDir\gen-test-glbs-draco.cpp" `
    "/Fe:$toolsDir\gen-test-glbs-draco.exe" /Fo:"$objDir\" `
    /link /SUBSYSTEM:CONSOLE `
    "/LIBPATH:$msvc\lib\x64" `
    "/LIBPATH:C:\Program Files (x86)\Windows Kits\10\Lib\$sdkVer\um\x64" `
    "/LIBPATH:C:\Program Files (x86)\Windows Kits\10\Lib\$sdkVer\ucrt\x64" `
    "/LIBPATH:$vcpkgInstalled\lib" `
    draco.lib
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }

# draco.dll must be next to the generator .exe (or on PATH) to actually run it.
Copy-Item "$vcpkgInstalled\bin\draco.dll" "$toolsDir\draco.dll" -Force

& "$toolsDir\gen-test-glbs-draco.exe"
if ($LASTEXITCODE -ne 0) { throw 'generation failed' }
