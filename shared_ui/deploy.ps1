param([Parameter(Position=0)][ValidateSet('build','build-preview','preview','clear-build','reset')][string]$Action)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot)
$repo = Split-Path -Parent $root
$ports = Get-Content -LiteralPath (Join-Path $repo '.vscode/preview-ports.json') -Raw | ConvertFrom-Json
$port = [int]$ports.shared_ui
if ($port -lt 1024 -or $port -gt 65535) { throw '❌ Invalid shared UI preview port.' }

function Assert-OwnedPath([string]$relative) {
    $target = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "❌ Unsafe cleanup target: $relative" }
    $current = $target
    while ($current.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "❌ Refusing linked cleanup target: $relative" }
        }
        $current = Split-Path -Parent $current
    }
    return $target
}
function Clear-Generated {
    foreach ($relative in @('dist','test-results','playwright-report','src/generated-validator.js','native/Sources/ThunderLinkUIResources/Resources','native/windows/generated')) {
        $target = Assert-OwnedPath $relative
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    }
}
function Ensure-Dependencies {
    Write-Host '⏳ Checking local UI dependencies'
    if (-not (Get-Command node -ErrorAction SilentlyContinue) -or -not (Get-Command npm -ErrorAction SilentlyContinue)) { throw '❌ Node 24 and npm are required for UI development.' }
    $version = (& node --version).Trim()
    if ($version -notmatch '^v24\.') { throw "❌ Node 24 is required; found $version" }
    $hash = (Get-FileHash (Join-Path $root 'package.json') -Algorithm SHA256).Hash + (Get-FileHash (Join-Path $root 'package-lock.json') -Algorithm SHA256).Hash + $version + (& npm --version).Trim()
    $stamp = Join-Path $root 'node_modules/.thunderlink-install-stamp'
    $ready = (Test-Path -LiteralPath $stamp) -and ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $hash)
    if ($ready) { & npm ls --depth=0 --silent *> $null; $ready = ($LASTEXITCODE -eq 0) }
    if (-not $ready) {
        Write-Host '⏳ Installing locked UI dependencies'
        & npm ci
        if ($LASTEXITCODE -ne 0) { throw '❌ npm ci failed.' }
        Set-Content -LiteralPath $stamp -Value $hash -NoNewline
    }
}
function Start-Preview {
    # The preview manager stops a previous task-owned server before it starts.
    & node scripts/preview-manager.mjs $port
    if ($LASTEXITCODE -ne 0) { throw '❌ Preview server failed.' }
}
try {
    Push-Location $root
    switch ($Action) {
        'build' { Ensure-Dependencies; Write-Host '🔨 Building shared UI'; & npm run build; if ($LASTEXITCODE -ne 0) { throw '❌ UI build failed.' }; Write-Host '✅ Shared UI ready' }
        'build-preview' { Ensure-Dependencies; Write-Host '🔨 Building shared UI'; & npm run build; if ($LASTEXITCODE -ne 0) { throw '❌ UI build failed.' }; Start-Preview }
        'preview' { Ensure-Dependencies; if (-not (Test-Path -LiteralPath (Join-Path $root 'dist/manifest.json'))) { throw '❌ No built UI. Run Build & Preview first.' }; Start-Preview }
        'clear-build' { Write-Host '🧹 Clearing generated UI output'; Clear-Generated; Write-Host '✅ Cleared generated UI output' }
        'reset' { Write-Host '♻️ Resetting local UI dependencies and output'; Clear-Generated; $deps = Assert-OwnedPath 'node_modules'; if (Test-Path -LiteralPath $deps) { Remove-Item -LiteralPath $deps -Recurse -Force }; Write-Host '✅ Reset complete' }
    }
} catch { [Console]::Error.WriteLine("❌ " + $_.Exception.Message); exit 1 } finally { Pop-Location }
