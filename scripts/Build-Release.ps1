$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$version = (Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version.' }
$releaseName = 'NSU-net-autologin-v' + $version
$buildRoot = Join-Path $projectRoot 'build'
$stage = Join-Path $buildRoot ([guid]::NewGuid().ToString())
$packageRoot = Join-Path $stage $releaseName
$dist = Join-Path $projectRoot 'dist'
$files = @('CampusLogin.ps1', 'CampusLogin.Core.ps1', 'Start.cmd', 'Configure.cmd', 'Install.cmd', 'Uninstall.cmd', 'TestLogin.cmd', 'README.md', 'VERSION', 'CHANGELOG.md', 'docs\FUNCTIONS.md')
try {
    [void][IO.Directory]::CreateDirectory($packageRoot)
    [void][IO.Directory]::CreateDirectory($dist)
    # Only these reviewed files may enter the downloadable package.
    foreach ($relative in $files) {
        $target = Join-Path $packageRoot $relative
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
        Copy-Item -LiteralPath (Join-Path $projectRoot $relative) -Destination $target
    }
    $archive = Join-Path $dist ($releaseName + '.zip')
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive -Force
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    ($hash + '  ' + [IO.Path]::GetFileName($archive)) | Set-Content -LiteralPath ($archive + '.sha256') -Encoding ASCII
    Write-Output $archive
    Write-Output ($archive + '.sha256')
} finally {
    # Verify the resolved temporary directory stays inside this project's build folder.
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $allowedPrefix = [IO.Path]::GetFullPath($buildRoot) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedStage.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected staging directory.' }
    if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
}
