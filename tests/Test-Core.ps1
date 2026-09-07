$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\CampusLogin.Core.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) 'NSU-net-autologin-tests'
$script:CampusDataDir = Join-Path $testRoot ([guid]::NewGuid().ToString())
$script:checks = 0

try {

# Fail a regression immediately while counting successful assertions.
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}

$vectors = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'des-vectors.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($vector in $vectors) {
    $secure = [Security.SecureString]::new()
    foreach ($character in $vector.password.ToCharArray()) { $secure.AppendChar($character) }
    $credential = [pscredential]::new($vector.username, $secure)
    Assert-True ((ConvertTo-WebCCPassword $credential) -ceq $vector.expected) 'DES encoding mismatch.'
}
$dummy = [pscredential]::new('2024123456', (ConvertTo-SecureString 'DUMMY-PASSWORD-ONLY-9827' -AsPlainText -Force))
$config = [pscustomobject]@{ Version=2; Operator='移动'; PackageName=''; Credential=$dummy }
Save-CampusConfig $config
$restored = Read-CampusConfig
Assert-True ($restored.Credential.GetNetworkCredential().Password -eq 'DUMMY-PASSWORD-ONLY-9827') 'DPAPI roundtrip failed.'
$stored = Get-Content -LiteralPath (Join-Path $script:CampusDataDir 'config.clixml') -Raw
Assert-True (-not $stored.Contains('DUMMY-PASSWORD-ONLY-9827')) 'Password saved as plaintext.'
$config.Operator='联通'
Save-CampusConfig $config
Assert-True ((Read-CampusConfig).Operator -eq '联通') 'Atomic configuration update failed.'
$config.Operator='移动'
$offline=[pscustomobject]@{IP='192.0.2.5';OIA=@();MOC=1;KXTC=@([pscustomobject]@{套餐名称='学生-移动-200M'},[pscustomobject]@{套餐名称='学生-联通-200M'},[pscustomobject]@{套餐名称='学生-电信-200M'})}
$online=[pscustomobject]@{IP='192.0.2.5';OIA=@([pscustomobject]@{IP='192.0.2.5'});MOC=1;KXTC=$offline.KXTC}
Assert-True (Test-WebCCOnline $online) 'Own online IP not recognized.'
Assert-True (-not (Test-WebCCOnline $offline)) 'Offline state reported as online.'
foreach ($operator in @('移动','联通','电信')) {
    $config.Operator=$operator
    Assert-True ((Select-WebCCPackage $offline $config).Contains($operator)) 'Operator matching failed.'
}
$config.Operator='移动'
$ambiguous=[pscustomobject]@{KXTC=@([pscustomobject]@{套餐名称='学生-移动-200M'},[pscustomobject]@{套餐名称='学生-移动-500M'})}
$thrown=$false
try { $null=Select-WebCCPackage $ambiguous $config } catch { $thrown=$true }
Assert-True $thrown 'Ambiguous package accepted.'
$config.PackageName='学生-移动-500M'
Assert-True ((Select-WebCCPackage $ambiguous $config) -eq $config.PackageName) 'Exact package failed.'
$config.PackageName='不存在'
$thrown=$false
try { $null=Select-WebCCPackage $offline $config } catch { $thrown=$true }
Assert-True $thrown 'Missing package accepted.'
$config.PackageName=''
Assert-True (Test-WebCCSuccess ([pscustomobject]@{Result=$true})) 'Boolean success rejected.'
foreach ($resultValue in @($false, 192, 1, 'true', 'wait', 'needLogin')) {
    Assert-True (-not (Test-WebCCSuccess ([pscustomobject]@{Result=$resultValue}))) 'Non-boolean result accepted as success.'
}
$thrown=$false
try { $null=Invoke-WebCC $null 'Logout' } catch { $thrown=$true }
Assert-True $thrown 'Unexpected logout action permitted.'

# Replace only the transport for deterministic state-machine tests; no real credentials are submitted.
function Invoke-WebCC($Client, [string]$Action, [hashtable]$Fields=@{}) {
    $script:calls.Add($Action)
    if ($script:responses.Count -eq 0) { throw 'Unexpected extra API call.' }
    return $script:responses.Dequeue()
}

# Remove waits from simulated transitions only.
function Start-Sleep { param([int]$Seconds) }

# Prepare a finite response queue and fresh action history for each scenario.
function Set-Scenario([object[]]$Responses) {
    $script:responses=[Collections.Generic.Queue[object]]::new()
    foreach($response in $Responses){$script:responses.Enqueue($response)}
    $script:calls=[Collections.Generic.List[string]]::new()
}
$ok=[pscustomobject]@{Result=$true}
$no=[pscustomobject]@{Result=$false}
$off=[pscustomobject]@{Result=$true;Data=$offline}
$on=[pscustomobject]@{Result=$true;Data=$online}
Set-Scenario @($ok,$on)
Invoke-CampusLogin $null $config
Assert-True (($script:calls -join ',') -eq 'Check,GetInfo') 'Already-online flow performed extra operations.'
Set-Scenario @($no,$ok,$off,$ok,$on)
Invoke-CampusLogin $null $config
Assert-True (($script:calls -join ',') -eq 'Check,Login,GetInfo,OpenNet,GetInfo') 'Login-package-online sequence wrong.'
Set-Scenario @($no,$no)
$thrown=$false
try { Invoke-CampusLogin $null $config } catch { $thrown=$true }
Assert-True ($thrown -and ($script:calls -join ',') -eq 'Check,Login') 'Bad password was retried or accepted.'
Set-Scenario @($ok,$off,[pscustomobject]@{Result=192;Token='dummy-token';Sec=5},[pscustomobject]@{Result='wait'},$ok,$on)
Invoke-CampusLogin $null $config
Assert-True (($script:calls -join ',') -eq 'Check,GetInfo,OpenNet,ReConnect,ReConnect,GetInfo') 'New-device reconnect flow failed.'
$full=[pscustomobject]@{IP='192.0.2.5';OIA=@([pscustomobject]@{IP='192.0.2.6'});MOC=1;KXTC=$offline.KXTC}
Set-Scenario @($ok,[pscustomobject]@{Result=$true;Data=$full})
$thrown=$false
try { Invoke-CampusLogin $null $config } catch { $thrown=$true }
Assert-True ($thrown -and -not $script:calls.Contains('OpenNet')) 'Device limit bypassed.'
Set-Scenario @($ok,[pscustomobject]@{Result=$true;Data=[pscustomobject]@{IP='';OIA=@()}})
$thrown=$false
try { Invoke-CampusLogin $null $config } catch { $thrown=$true }
Assert-True ($thrown -and -not $script:calls.Contains('OpenNet')) 'Malformed state accepted.'
Write-Output "PASS: $script:checks headless regression checks."
$definition = New-CampusStartupTask (Join-Path $PSScriptRoot '..\CampusLogin.ps1')
Assert-True ($definition.Triggers[0].CimClass.CimClassName -eq 'MSFT_TaskLogonTrigger') 'Startup trigger is not a logon trigger.'
Assert-True ([string]::IsNullOrEmpty($definition.Triggers[0].Delay)) 'Unexpected startup delay.'
Assert-True ($definition.Principal.LogonType -eq 3) 'Task does not use the interactive user session.'
Assert-True ($definition.Principal.RunLevel -eq 0) 'Task unexpectedly requests elevation.'
Assert-True (-not $definition.Settings.DisallowStartIfOnBatteries) 'Task disabled on battery.'
Assert-True (-not $definition.Settings.StopIfGoingOnBatteries) 'Task stops when switching to battery.'
Assert-True (-not $definition.Settings.RunOnlyIfNetworkAvailable) 'Scheduler incorrectly gates captive network startup.'
Assert-True ($definition.Settings.MultipleInstances -eq 2) 'Task allows duplicate instances.'
Assert-True ($definition.Actions[0].Arguments.EndsWith(' -Startup')) 'Startup mode argument missing.'
Assert-True ($definition.Settings.ExecutionTimeLimit -eq 'PT6M') 'Task execution limit changed.'
Write-Output "PASS: $script:checks total regression checks including startup task definition."
} finally {
    # Delete only this run's temporary test configuration, never the user's real data.
    $resolved = [IO.Path]::GetFullPath($script:CampusDataDir)
    $allowed = [IO.Path]::GetFullPath($testRoot) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected test directory.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
