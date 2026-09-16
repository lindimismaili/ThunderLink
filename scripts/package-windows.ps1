$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne 'Win32NT') { throw 'Build the receiver on native Windows.' }
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'packaging/VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid packaging version.' }
$build = Join-Path $repo 'windows_receiver/build-portable'
cmake -S (Join-Path $repo 'windows_receiver') -B $build -A x64 -DTLNK_PORTABLE=ON -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Windows build failed.' }
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Windows tests failed.' }
$exe = Join-Path $build 'Release/ThunderLink.exe'
# An ordinary Windows installation supplies these OS DLLs. MSVC runtime DLLs
# must not leak into the portable artifact.
$dependencies = & dumpbin /DEPENDENTS $exe
if ($LASTEXITCODE -ne 0) { throw 'Dependency inspection failed. Run from a Visual Studio developer shell.' }
if (($dependencies -join "`n") -match '(?i)(VCRUNTIME|MSVCP\d|MSVCR\d)') { throw 'Unexpected dynamic MSVC runtime dependency.' }
$systemDlls = @('kernel32.dll','kernelbase.dll','ntdll.dll','user32.dll','gdi32.dll','imm32.dll',
    'advapi32.dll','shell32.dll','shlwapi.dll','ole32.dll','oleaut32.dll','comdlg32.dll','combase.dll',
    'winmm.dll','setupapi.dll','dwmapi.dll','d3d12.dll','dxgi.dll','d3dcompiler_47.dll',
    'ws2_32.dll','bcrypt.dll','mf.dll','mfplat.dll','mfreadwrite.dll','ucrtbase.dll')
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
$stage = Join-Path ([IO.Path]::GetTempPath()) ('ThunderLink-' + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Copy-Item $exe (Join-Path $stage 'ThunderLink.exe')
    Copy-Item (Join-Path $repo 'packaging/licenses/THIRD-PARTY-NOTICES.txt') $stage
    $archive = Join-Path $dist "ThunderLink-$version-windows-x64.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force
    $hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -Path "$archive.sha256" -Value "$hash  $([IO.Path]::GetFileName($archive))"
    Write-Host "Created $archive and dist/ThunderLink.exe"
} finally { Remove-Item -Recurse -Force $stage }
