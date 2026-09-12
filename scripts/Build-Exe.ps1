param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist\v1.0'))
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($outputRoot)
# Compile real C machine code. This script is used only on the build machine.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$sources = @('main.c', 'system.c', 'network.c') | ForEach-Object { Join-Path $projectRoot ('app\' + $_) }
Push-Location $outputRoot
try {
    & rc.exe /nologo /fo version.res (Join-Path $projectRoot 'app\version.rc')
    if ($LASTEXITCODE -ne 0) { throw 'Version resource compilation failed.' }
    foreach ($name in @('NSU-Net-Autologin.exe')) {
        $arguments = @('/nologo', '/TC', '/std:c11', '/utf-8', '/W4', '/O2', '/MT', '/GS', '/guard:cf', '/D_CRT_SECURE_NO_WARNINGS', ('/Fe:' + $name))
        & cl.exe @arguments @sources /link version.res /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /guard:cf user32.lib gdi32.lib shell32.lib advapi32.lib crypt32.lib winhttp.lib ole32.lib oleaut32.lib uuid.lib taskschd.lib
        if ($LASTEXITCODE -ne 0) { throw "Native compilation failed: $name" }
        Write-Output (Join-Path $outputRoot $name)
    }
    # Remove only named compiler intermediates; the deliverable remains in the output directory.
    foreach ($intermediate in @('main.obj', 'system.obj', 'network.obj', 'version.res', 'NSU-Net-Autologin.exe.manifest')) {
        $intermediatePath = Join-Path $outputRoot $intermediate
        if (Test-Path -LiteralPath $intermediatePath) { Remove-Item -LiteralPath $intermediatePath -Force }
    }
} finally { Pop-Location }
