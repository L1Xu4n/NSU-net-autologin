param([switch]$Configure, [switch]$Install, [switch]$Uninstall, [switch]$CheckOnly, [switch]$Force)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'CampusLogin.Core.ps1')
# Force 仅为兼容旧测试入口；新版始终按校园网接口状态判断是否需要登录。
$result = Start-CampusApp -ScriptPath $PSCommandPath -Configure $Configure.IsPresent -Install $Install.IsPresent -Uninstall $Uninstall.IsPresent -CheckOnly $CheckOnly.IsPresent
exit $result
