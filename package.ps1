# package.ps1 — упаковывает релизный архив VCFLister для установки в Total Commander
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$stateFile = Join-Path $root "build_state.json"
$ver = "2.5.4"
if (Test-Path $stateFile) {
    try {
        $st = Get-Content $stateFile -Raw | ConvertFrom-Json
        if ($st.base) { $ver = $st.base }
    } catch {}
}

$wlx32 = Join-Path $root "Release\VCFLister.wlx"
$wlx64 = Join-Path $root "x64\Release\VCFLister.wlx64"
$inf = Join-Path $root "pluginst.inf"

if (-not (Test-Path $wlx32)) {
    throw "File not found: $wlx32. Run Release build first."
}
if (-not (Test-Path $wlx64)) {
    throw "File not found: $wlx64. Run x64 Release build first."
}
if (-not (Test-Path $inf)) {
    throw "File not found: $inf."
}

$staging = Join-Path $env:TEMP "VCFLister_package_$PID"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Path $staging | Out-Null

try {
    Copy-Item $wlx32 -Destination $staging
    Copy-Item $wlx64 -Destination $staging
    Copy-Item $inf -Destination $staging

    $docs = @("README.md", "CHANGELOG.md", "LICENSE.txt")
    foreach ($doc in $docs) {
        $docPath = Join-Path $root $doc
        if (Test-Path $docPath) {
            Copy-Item $docPath -Destination $staging
        }
    }

    $zipName = "VCFLister-$ver.zip"
    $zipPath = Join-Path $root $zipName
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($staging, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)

    Write-Host "Created plugin package: $zipPath"
    
    # Also verify contents
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    Write-Host "Archive contents:"
    foreach ($entry in $archive.Entries) {
        Write-Host ("  {0,10} bytes  {1}" -f $entry.Length, $entry.FullName)
    }
    $archive.Dispose()
}
finally {
    if (Test-Path $staging) {
        Remove-Item -Recurse -Force $staging
    }
}
