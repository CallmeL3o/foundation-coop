# Drift test with a held preview: fresh session, step, then 4 rounds of play + savecheck (continues past differences).
param([string]$Step = "client_hold_market.ps1", [int]$Rounds = 4, [int]$Interval = 45)
$bot = $PSScriptRoot
& (Join-Path $bot "run_ab_test.ps1") -RoleA host -RoleB client -WaitAfter 3 | Out-Null
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
& (Join-Path $bot "ui.ps1") -GamePid $idA -Action key -Key 0x33 | Out-Null
& (Join-Path $bot $Step)
for ($r = 1; $r -le $Rounds; $r++) {
    Start-Sleep $Interval
    & (Join-Path $bot "savecheck.ps1") -Tag "round$r" -Resume 2>&1 | Select-String "host:|client:|differing simulation|RESULT" | ForEach-Object { $_.Line }
}
"desyncs: " + (Select-String -Path E:\FoundationCoopTest\instances\B\coop.log -Pattern "DESYNC" -CaseSensitive | Measure-Object).Count
Select-String -Path E:\FoundationCoopTest\instances\A\coop.log -Pattern "left on the thread pool" | ForEach-Object { $_.Line }
