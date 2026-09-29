$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne 'Win32NT') { throw 'Build the receiver on native Windows.' }
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'packaging/VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid packaging version.' }
$build = Join-Path $repo 'windows_receiver/build-portable'
& powershell.exe -NoProfile -File (Join-Path $repo 'shared_ui/deploy.ps1') build
if ($LASTEXITCODE -ne 0) { throw 'Shared UI preparation failed.' }
cmake -S (Join-Path $repo 'windows_receiver') -B $build -A x64 -DTLNK_PORTABLE=ON -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Windows build failed.' }
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Windows tests failed.' }
$exe = Join-Path $build 'Release/ThunderLink.exe'
# An ordinary Windows installation supplies these OS DLLs. MSVC runtime DLLs
# must not leak into the portable artifact.
$dumpbin = (Get-Command dumpbin -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $dumpbin) {
    $linkerLine = Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern '^CMAKE_LINKER:FILEPATH=' | Select-Object -First 1
    if ($linkerLine) {
        $linker = $linkerLine.Line.Substring($linkerLine.Line.IndexOf('=') + 1)
        $candidate = Join-Path (Split-Path -Parent $linker) 'dumpbin.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $dumpbin = $candidate }
    }
}
if (-not $dumpbin) { throw 'Cannot locate dumpbin.exe beside the CMake linker. Install Visual Studio C++ tools or run from a developer shell.' }
$dependencies = & $dumpbin /DEPENDENTS $exe
if ($LASTEXITCODE -ne 0) { throw 'Dependency inspection failed. Run from a Visual Studio developer shell.' }
if (($dependencies -join "`n") -match '(?i)(VCRUNTIME|MSVCP\d|MSVCR\d)') { throw 'Unexpected dynamic MSVC runtime dependency.' }
$systemDlls = @('kernel32.dll','kernelbase.dll','ntdll.dll','user32.dll','gdi32.dll','imm32.dll',
    'advapi32.dll','shell32.dll','shlwapi.dll','ole32.dll','oleaut32.dll','comdlg32.dll','combase.dll',
    'winmm.dll','setupapi.dll','dwmapi.dll','d3d12.dll','dxgi.dll','d3dcompiler_47.dll',
    'ws2_32.dll','bcrypt.dll','ncrypt.dll','crypt32.dll','secur32.dll','dnsapi.dll',
    'iphlpapi.dll','wtsapi32.dll','mf.dll','mfplat.dll','mfreadwrite.dll','ucrtbase.dll')
foreach ($line in $dependencies) {
    if ($line.Trim() -match '^([A-Za-z0-9_.-]+\.dll)$') {
        $dll = $Matches[1].ToLowerInvariant()
        if ($dll -notin $systemDlls -and $dll -notmatch '^(api-ms-win-|ext-ms-win-)') {
            throw "Unbundled non-system dependency: $dll"
        }
    }
}

$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item $exe (Join-Path $dist 'ThunderLink.exe') -Force
$assetVerifier = Join-Path $build 'tests/receiver-tests/Release/UIAssetStoreTests.exe'
& $assetVerifier (Join-Path $dist 'ThunderLink.exe')
if ($LASTEXITCODE -ne 0) { throw 'Packaged executable UI resources failed verification.' }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('ThunderLink-' + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Copy-Item $exe (Join-Path $stage 'ThunderLink.exe')
    Copy-Item (Join-Path $repo 'packaging/licenses/THIRD-PARTY-NOTICES.txt') $stage
    Copy-Item (Join-Path $repo 'shared_ui/licenses/Inter-OFL.txt') $stage
    Copy-Item (Join-Path $repo 'shared_ui/licenses/Lit-BSD-3-Clause.txt') $stage
    Copy-Item (Join-Path $repo 'shared_ui/dist/manifest.json') (Join-Path $stage 'ui-manifest.json')
    Copy-Item (Join-Path $repo 'packaging/VERSION') (Join-Path $stage 'VERSION')
    $archive = Join-Path $dist "ThunderLink-$version-windows-x64.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force
    $hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -Path "$archive.sha256" -Value "$hash  $([IO.Path]::GetFileName($archive))"
    Write-Host "Created $archive and dist/ThunderLink.exe"
} finally { Remove-Item -Recurse -Force $stage }
