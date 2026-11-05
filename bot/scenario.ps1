# Lockstep scenario test: fresh two-instance session (host + client, shared start save), optional action steps,
# play for -PlaySeconds, then savecheck.ps1 (fingerprints + field-by-field save comparison).
# usage: scenario.ps1 -Name soak -Steps @(".\step1.ps1", ...) -PlaySeconds 300 [-HostSpeed 3]
param([string]$Name = "scenario", [string[]]$Steps = @(), [int]$PlaySeconds = 120, [int]$HostSpeed = 1, [int]$ClientSpeed = 0)
$bot = $PSScriptRoot
$ui = Join-Path $bot "ui.ps1"
"=== scenario $Name ($(Get-Date -Format HH:mm:ss))"
& (Join-Path $bot "run_ab_test.ps1") -RoleA host -RoleB client -WaitAfter 3 | Out-Null
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
if ($HostSpeed -gt 1) { & $ui -GamePid $idA -Action key -Key ("0x3" + $HostSpeed) | Out-Null }
foreach ($s in $Steps) { "--- step $s"; & (Join-Path $bot $s) }
if ($ClientSpeed -gt 0) { Start-Sleep 20; & $ui -GamePid $idB -Action key -Key ("0x3" + $ClientSpeed) | Out-Null; "client pressed speed x$ClientSpeed" }
Start-Sleep $PlaySeconds
& (Join-Path $bot "savecheck.ps1") -Tag $Name
"--- client log"
Select-String -Path E:\FoundationCoopTest\instances\B\coop.log -Pattern "DESYNC|own command|malformed|mismatch|none is pending" -CaseSensitive | Select-Object -Last 8 | ForEach-Object { $_.Line }
"starved ids: " + (Select-String -Path E:\FoundationCoopTest\instances\B\coop.log -Pattern "no host id" -CaseSensitive | Measure-Object).Count
"=== end $Name ($(Get-Date -Format HH:mm:ss))"
