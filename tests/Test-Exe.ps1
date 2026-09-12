param([string]$ExeDirectory = (Join-Path $PSScriptRoot '..\dist\v1.0'))
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('NSU-native-tests-' + [guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($root)
$taskName = 'NSU-net-autologin-' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$existingTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
$beforeTask = if ($existingTask) { Export-ScheduledTask -TaskName $taskName } else { $null }
try {
    $key = [Text.Encoding]::UTF8.GetBytes('3456202312345612345678')[0..7]
    $des = [Security.Cryptography.DES]::Create()
    $des.Mode = 'ECB'; $des.Padding = 'PKCS7'; $des.Key = $key
    $transform = $des.CreateEncryptor()
    $plain = [Text.Encoding]::UTF8.GetBytes('test123')
    $expected = [BitConverter]::ToString($transform.TransformFinalBlock($plain, 0, $plain.Length)).Replace('-', '').ToLowerInvariant()
    $transform.Dispose(); $des.Dispose()
    foreach ($name in @('NSU-Net-Autologin.exe')) {
        $path = Join-Path $root $name
        Copy-Item -LiteralPath (Join-Path $ExeDirectory $name) -Destination $path
        $expectedVersion = (Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\VERSION') -Raw).Trim()
        if ([Diagnostics.FileVersionInfo]::GetVersionInfo($path).ProductVersion -ne $expectedVersion) { throw 'EXE version does not match VERSION.' }
        # A native PE has no CLR header. Also reject embedded PowerShell runtime/source names.
        $bytes = [IO.File]::ReadAllBytes($path)
        $pe = [BitConverter]::ToInt32($bytes, 0x3c)
        $optional = $pe + 24
        if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20b) { throw 'Expected x64 native PE.' }
        if ([BitConverter]::ToUInt32($bytes, $optional + 112 + 14 * 8) -ne 0) { throw 'Unexpected CLR runtime.' }
        $ascii = [Text.Encoding]::ASCII.GetString($bytes)
        if ($ascii -match 'System.Management.Automation|CampusLogin.Core.ps1|powershell.exe') { throw 'Unexpected script payload.' }
        $start = [Diagnostics.ProcessStartInfo]::new($path, '--self-test')
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($start)
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Native self-test timeout.' }
        $actual = $process.StandardOutput.ReadToEnd()
        $diagnostic = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne 0 -or $actual -ne $expected) { throw "Native protocol/DES/task validation failed: $name (exit $($process.ExitCode), vector $actual): $diagnostic" }
        $process.Dispose()
        # Exercise actual Win32 controls and render only app-owned windows with fictional success.
        $start.Arguments = '--ui-test'; $start.WorkingDirectory = $root
        $process = [Diagnostics.Process]::Start($start)
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'UI test timeout.' }
        if ($process.ExitCode -ne 0) { throw "Native UI/notification test failed: $name" }
        $process.Dispose()
        $preview = Join-Path $PSScriptRoot '..\work\native-preview'
        [void][IO.Directory]::CreateDirectory($preview)
        Copy-Item -LiteralPath (Join-Path $root 'native-menu.bmp') -Destination (Join-Path $preview ($name + '.bmp')) -Force
        Copy-Item -LiteralPath (Join-Path $root 'native-toast.bmp') -Destination (Join-Path $preview 'toast.bmp') -Force
        Copy-Item -LiteralPath (Join-Path $root 'native-wait.bmp') -Destination (Join-Path $preview 'wait.bmp') -Force
        Copy-Item -LiteralPath (Join-Path $root 'native-about.bmp') -Destination (Join-Path $preview 'about.bmp') -Force
        Copy-Item -LiteralPath (Join-Path $root 'native-phone.bmp') -Destination (Join-Path $preview 'phone.bmp') -Force
        Copy-Item -LiteralPath (Join-Path $root 'native-menu-phone.bmp') -Destination (Join-Path $preview 'menu-phone.bmp') -Force
        Copy-Item -LiteralPath (Join-Path $root 'native-menu-installed.bmp') -Destination (Join-Path $preview 'menu-installed.bmp') -Force
        $start.Arguments = '--ui-test-large'
        $process = [Diagnostics.Process]::Start($start)
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Large UI test timeout.' }
        if ($process.ExitCode -ne 0) { throw 'Scaled phone notification layout failed.' }
        $process.Dispose()
        Copy-Item -LiteralPath (Join-Path $root 'native-phone.bmp') -Destination (Join-Path $preview 'phone-large.bmp') -Force
    }
    $afterTask = if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) { Export-ScheduledTask -TaskName $taskName } else { $null }
    if ($beforeTask -cne $afterTask) { throw 'Self-test changed the real scheduled task.' }
    Write-Output 'PASS: standalone native EXE; protocol/DES/DPAPI; isolated task install/update/persisted-definition/query/uninstall/uninstall-again; real task unchanged; guided vertical UI; colored phone notice at normal and 150% size.'
} finally {
    # Remove only the GUID directory created by this test; never delete real user data.
    $resolved = [IO.Path]::GetFullPath($root)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\NSU-native-tests-'
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected test root.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
