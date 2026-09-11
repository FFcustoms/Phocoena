<##
Keep only the newest verified installation release in dist. Dry-run by default.
Old source snapshots/manifests go under build/release-history for debugging;
superseded SD ZIPs and staged NRO folders are deleted. Never touches the SD card.
##>
param(
    [Parameter(Mandatory = $true)][string]$KeepVersion,
    [switch]$Apply,
    [string]$ProjectRoot = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Windows PowerShell can evaluate parameter defaults before PSScriptRoot is set.
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
if ($KeepVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version' }
$projectPath = [IO.Path]::GetFullPath($ProjectRoot)
$distPath = [IO.Path]::GetFullPath((Join-Path $projectPath 'dist'))
$historyPath = [IO.Path]::GetFullPath((Join-Path $projectPath 'build/release-history'))
foreach ($parent in @($projectPath, $distPath, (Join-Path $projectPath 'build'), $historyPath)) {
    if ((Test-Path -LiteralPath $parent) -and
        ((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing linked parent: $parent"
    }
}

function Assert-Inside([string]$Path, [string]$Directory) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $prefix = $Directory.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside intended directory: $absolute"
    }
    return $absolute
}

function Get-SafeTreeFiles([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing linked path: $Path" }
    if ($item.PSIsContainer) {
        foreach ($child in Get-ChildItem -LiteralPath $Path -Force) {
            Get-SafeTreeFiles $child.FullName
        }
    } else { $item.FullName }
}

function Assert-Hash([string]$Path, [string]$Expected) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {
        throw "File changed; refusing cleanup: $Path"
    }
}

$keeper = Assert-Inside (Join-Path $distPath "v$KeepVersion") $distPath
$report = Get-Content -LiteralPath (Join-Path $keeper 'RELEASE-VERIFICATION.json') -Raw | ConvertFrom-Json
if ($report.version -ne $KeepVersion -or $report.zip_crc -ne 'PASS') { throw 'New release has not passed verification' }
$required = @("dist/v$KeepVersion/switch/dolphin/dolphin.nro",
              "dist/dolphin-horizon-v$KeepVersion.zip", "dist/dolphin-horizon-v$KeepVersion-source.tar.gz")
foreach ($relative in $required) {
    $file = Assert-Inside (Join-Path $projectPath $relative) $distPath
    $null = @(Get-SafeTreeFiles $file)
    $record = $report.artifacts.PSObject.Properties[$relative]
    if (-not $record) { throw "Missing verification record: $relative" }
    Assert-Hash $file $record.Value.sha256
}

$actions = [Collections.Generic.List[object]]::new()
foreach ($item in Get-ChildItem -LiteralPath $distPath -Force) {
    $oldVersion = $null
    $kind = $null
    if ($item.PSIsContainer -and $item.Name -match '^v(\d+\.\d+(?:\.\d+)?)$') {
        $oldVersion = $Matches[1]; $kind = 'stage'
    } elseif (-not $item.PSIsContainer -and $item.Name -match '^dolphin-horizon-v(\d+\.\d+(?:\.\d+)?)(-source\.tar\.gz|\.zip)$') {
        $oldVersion = $Matches[1]
        $kind = if ($Matches[2] -eq '.zip') { 'zip' } else { 'source' }
    } elseif ($item.Name -eq 'switch' -and $item.PSIsContainer) {
        # Legacy v0.1 layout, identified by its own generated manifest.
        $legacy = Get-Content -LiteralPath (Join-Path $item.FullName 'dolphin/BUILD-MANIFEST.json') -Raw | ConvertFrom-Json
        if ($legacy.name -ne 'Dolphin for Switch') { throw 'Unrecognized legacy dist/switch; leave it alone' }
        $oldVersion = '0.1.0'; $kind = 'legacy-stage'
    }
    if (-not $oldVersion) { continue }
    if (([version]$oldVersion).Build -lt 0) { $oldVersion += '.0' }
    if ([version]$oldVersion -ge [version]$KeepVersion) { continue }
    $path = Assert-Inside $item.FullName $distPath
    $files = @(Get-SafeTreeFiles $path)
    if ($kind -in @('stage', 'legacy-stage')) {
        $stage = if ($kind -eq 'stage') { Join-Path $path 'switch/dolphin' } else { Join-Path $path 'dolphin' }
        $manifestPath = Join-Path $stage 'BUILD-MANIFEST.json'
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        if ($manifest.name -ne 'Dolphin for Switch') { throw "Unrecognized staged application: $path" }
        foreach ($file in $files) {
            if ($file -eq $manifestPath -or $file -eq (Join-Path $path 'RELEASE-VERIFICATION.json')) { continue }
            $null = Assert-Inside $file $stage
            $relative = $file.Substring($stage.Length + 1).Replace('\', '/')
            $record = $manifest.files.PSObject.Properties[$relative]
            if (-not $record) { throw "Unrecognized file in old release; refusing deletion: $file" }
            Assert-Hash $file $record.Value
        }
    }
    $actions.Add([pscustomobject]@{Path=$path; Version=$oldVersion; Kind=$kind})
}

# Preflight every destination as well as every deletion before doing any writes.
foreach ($action in $actions) {
    $destination = Assert-Inside (Join-Path $historyPath $action.Version) $historyPath
    if (Test-Path -LiteralPath $destination) { $null = @(Get-SafeTreeFiles $destination) }
    if ($action.Kind -eq 'source') {
        $archive = Join-Path $destination 'source.tar.gz'
        if (Test-Path -LiteralPath $archive) { Assert-Hash $archive (Get-FileHash -LiteralPath $action.Path).Hash }
    }
}
$actions | Select-Object Kind, Version, Path | Format-Table -AutoSize
if (-not $Apply) { Write-Output 'Dry run only. Use -Apply after reviewing this list.'; exit 0 }

foreach ($action in $actions) {
    $path = Assert-Inside $action.Path $distPath
    $null = @(Get-SafeTreeFiles $path)
    $destination = Assert-Inside (Join-Path $historyPath $action.Version) $historyPath
    $null = New-Item -ItemType Directory -Path $destination -Force
    if ($action.Kind -eq 'source') {
        $archive = Join-Path $destination 'source.tar.gz'
        if (-not (Test-Path -LiteralPath $archive)) {
            Move-Item -LiteralPath $path -Destination $archive
            continue
        }
        Assert-Hash $path (Get-FileHash -LiteralPath $archive).Hash
    } elseif ($action.Kind -in @('stage', 'legacy-stage')) {
        $stage = if ($action.Kind -eq 'stage') { Join-Path $path 'switch/dolphin' } else { Join-Path $path 'dolphin' }
        Copy-Item -LiteralPath (Join-Path $stage 'BUILD-MANIFEST.json') -Destination $destination
        $oldReport = if ($action.Kind -eq 'stage') { Join-Path $path 'RELEASE-VERIFICATION.json' } else { Join-Path $distPath 'RELEASE-VERIFICATION.json' }
        if (Test-Path -LiteralPath $oldReport) {
            Copy-Item -LiteralPath $oldReport -Destination $destination
            if ($action.Kind -eq 'legacy-stage') { Remove-Item -LiteralPath $oldReport }
        }
    }
    # All paths were checked above; deletion uses native PowerShell only.
    Remove-Item -LiteralPath $path -Recurse -Force
}
Write-Output "Kept v$KeepVersion in dist. Debug history is under build/release-history."
