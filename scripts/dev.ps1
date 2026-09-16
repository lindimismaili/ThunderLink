#Requires -Version 5.1
<#
.SYNOPSIS
    Windows half of the ThunderLink VS Code "Build" and "Run" tasks.

.DESCRIPTION
    build : verify Git, CMake 3.20+ and the MSVC C++ toolchain (offering to
            install anything missing), configure CMake once, then compile the
            receiver.
    run   : compile only when a source file is newer than the executable, then
            launch the receiver and stay attached until it exits.

    The macOS sender is driven by the sibling scripts/dev.sh.
#>
[CmdletBinding()]
param(
    [ValidateSet('build', 'run')]
    [string] $Task = 'build',

    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug',

    # Install missing tools without asking. Also honoured via TLNK_AUTO_INSTALL=1.
    [switch] $AutoInstall,

    # Everything after the named parameters is forwarded to ThunderLink.exe.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $AppArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'The ThunderLink receiver builds and runs only on native Windows. Use scripts/dev.sh on macOS.'
}
if (-not $AppArgs) { $AppArgs = @() }

$Repo     = Split-Path -Parent $PSScriptRoot
$Source   = Join-Path $Repo 'windows_receiver'
$BuildDir = Join-Path $Source 'build'
$Exe      = Join-Path $BuildDir "$Config\ThunderLink.exe"
$script:CMakeExe = $null

function Write-Step([string] $Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Note([string] $Message) { Write-Host "    $Message" -ForegroundColor DarkGray }

# winget records new tool locations in the registry, not in this process.
function Update-SessionPath {
    $parts = @(
        [Environment]::GetEnvironmentVariable('Path', 'Machine'),
        [Environment]::GetEnvironmentVariable('Path', 'User')
    ) | Where-Object { $_ }
    if ($parts) { $env:Path = $parts -join ';' }
}

function Confirm-Install([string] $Name, [string] $Command, [string] $Reason) {
    if ($AutoInstall -or $env:TLNK_AUTO_INSTALL -eq '1') { return $true }
    if (-not [Environment]::UserInteractive) { return $false }
    Write-Host ''
    Write-Host "$Name is required but was not found on this PC." -ForegroundColor Yellow
    if ($Reason) { Write-Note $Reason }
    Write-Note "Proposed command: $Command"
    $reply = Read-Host 'Install it now? [y/N]'
    return ($reply -match '^\s*(y|yes)\s*$')
}

function Install-WithWinget {
    param(
        [string]   $Name,
        [string]   $PackageId,
        [string]   $Reason,
        [string[]] $ExtraArgs = @()
    )

    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "$Name is missing and winget is unavailable on this PC. Install $Name manually (see docs/SETUP.md), then re-run the task."
    }

    $arguments = @('install', '--id', $PackageId, '--exact',
                   '--accept-package-agreements', '--accept-source-agreements')
    # --silent and --override both steer the vendor installer; winget rejects both together.
    if ($ExtraArgs -notcontains '--override') { $arguments += '--silent' }
    $arguments += $ExtraArgs

    if (-not (Confirm-Install $Name ('winget ' + ($arguments -join ' ')) $Reason)) {
        throw "$Name is required. Install it manually, or re-run the task with -AutoInstall (or set TLNK_AUTO_INSTALL=1)."
    }

    Write-Step "Installing $Name (this can take a while)"
    & winget @arguments
    # 0x8A150010: the package installed but a reboot is pending.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335216) {
        throw "winget could not install $Name (exit code $LASTEXITCODE)."
    }
    Update-SessionPath
}

function Get-VsWherePath {
    $path = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $path) { return $path }
    return $null
}

function Get-VisualStudioRoots([string[]] $Requires = @()) {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) { return @() }
    $arguments = @('-products', '*', '-latest', '-prerelease', '-property', 'installationPath')
    foreach ($component in $Requires) { $arguments += @('-requires', $component) }
    try { $output = & $vswhere @arguments 2>$null } catch { return @() }
    return @($output | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
}

function Test-CMakeVersion([string] $Path) {
    try { $output = & $Path '--version' 2>$null } catch { return $false }
    $match = [regex]::Match(($output -join "`n"), 'cmake version (\d+)\.(\d+)')
    if (-not $match.Success) { return $false }
    $found = New-Object System.Version ([int] $match.Groups[1].Value), ([int] $match.Groups[2].Value)
    return $found -ge [version]'3.20'
}

# PATH first, then the two places CMake commonly lands without joining PATH.
function Resolve-CMake {
    $candidates = @()
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    $candidates += (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe')
    foreach ($root in (Get-VisualStudioRoots)) {
        $candidates += (Join-Path $root 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate) -and (Test-CMakeVersion $candidate)) {
            return $candidate
        }
    }
    return $null
}

function Initialize-Toolchain {
    Write-Step 'Checking the Windows build toolchain'

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Install-WithWinget -Name 'Git' -PackageId 'Git.Git' `
            -Reason 'CMake FetchContent clones Dear ImGui with Git during the first configure.'
    }

    $vcComponent = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    # PowerShell unwraps single-element arrays on return, so re-wrap at the call site.
    $vcRoots = @(Get-VisualStudioRoots @($vcComponent))
    if ($vcRoots.Count -eq 0) {
        Install-WithWinget -Name 'Visual Studio 2022 Build Tools (Desktop development with C++)' `
            -PackageId 'Microsoft.VisualStudio.2022.BuildTools' `
            -Reason 'Several GB. Supplies the MSVC compiler and the Windows SDK that D3D12 and Media Foundation need.' `
            -ExtraArgs @('--override', '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
        $vcRoots = @(Get-VisualStudioRoots @($vcComponent))
        if ($vcRoots.Count -eq 0) {
            throw 'The MSVC C++ toolchain is still missing. Finish the Visual Studio installer, then re-run the task.'
        }
    }

    $script:CMakeExe = Resolve-CMake
    if (-not $script:CMakeExe) {
        Install-WithWinget -Name 'CMake 3.20 or newer' -PackageId 'Kitware.CMake' `
            -Reason 'The receiver is a CMake project; see docs/SETUP.md.'
        $script:CMakeExe = Resolve-CMake
        if (-not $script:CMakeExe) {
            throw 'CMake 3.20+ is still not resolvable. Open a fresh terminal (or reboot) and re-run the task.'
        }
    }

    Write-Note "cmake  $script:CMakeExe"
    Write-Note "msvc   $($vcRoots[0])"
}

# The linker cannot replace an executable that is still running.
function Stop-Receiver {
    $running = @(Get-Process -Name 'ThunderLink' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        Write-Note 'Stopping the running receiver so the linker can replace ThunderLink.exe.'
        $running | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 300
    }
}

function Get-NewestWriteTime([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Get-Item -LiteralPath $Path).LastWriteTimeUtc
    }
    $newest = Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($newest) { return $newest.LastWriteTimeUtc }
    return $null
}

function Test-ReceiverUpToDate {
    if (-not (Test-Path -LiteralPath $Exe)) { return $false }
    $builtAt = (Get-Item -LiteralPath $Exe).LastWriteTimeUtc
    # common/ is half of the wire contract, so a change there invalidates the build too.
    $inputs = @(
        (Join-Path $Source 'src'),
        (Join-Path $Source 'shaders'),
        (Join-Path $Source 'CMakeLists.txt'),
        (Join-Path $Repo 'common'),
        (Join-Path $Repo 'packaging\windows'),
        (Join-Path $Repo 'packaging\VERSION')
    )
    foreach ($item in $inputs) {
        $changedAt = Get-NewestWriteTime $item
        if ($changedAt -and $changedAt -gt $builtAt) { return $false }
    }
    return $true
}

function Invoke-Build {
    Initialize-Toolchain

    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
        Write-Step 'Configuring CMake'
        Write-Note 'The first configure downloads nlohmann/json and Dear ImGui, so it needs internet access.'
        & $script:CMakeExe -S $Source -B $BuildDir -A x64
        if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    }

    Stop-Receiver
    Write-Step "Building ThunderLink.exe ($Config)"
    & $script:CMakeExe --build $BuildDir --config $Config --target ThunderLinkReceiver --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    Write-Host "Built $Exe" -ForegroundColor Green
}

function Invoke-Run {
    if (Test-ReceiverUpToDate) {
        Write-Step 'Build is already current; skipping compilation'
    } else {
        Invoke-Build
    }

    Write-Step 'Starting the ThunderLink receiver'
    Write-Note 'Allow it through Windows Firewall on the direct-link profile: UDP 50001 and TCP 50003 inbound.'
    $startArgs = @{ FilePath = $Exe; WorkingDirectory = $Repo; PassThru = $true }
    if ($AppArgs.Count -gt 0) { $startArgs['ArgumentList'] = $AppArgs }
    $process = Start-Process @startArgs
    $process.WaitForExit()
    Write-Note "Receiver exited with code $($process.ExitCode)."
    exit $process.ExitCode
}

switch ($Task) {
    'build' { Invoke-Build }
    'run'   { Invoke-Run }
}
