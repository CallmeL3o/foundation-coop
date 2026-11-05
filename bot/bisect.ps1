# Drift hunt: fresh session, optional steps, then repeated short play + savecheck until the saves differ.
param([string[]]$Steps = @(), [int]$Interval = 30, [int]$Rounds = 6, [int]$HostSpeed = 3)
$bot = $PSScriptRoot
$ui = Join-Path $bot "ui.ps1"
& (Join-Path $bot "run_ab_test.ps1") -RoleA host -RoleB client -WaitAfter 3 | Out-Null
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
if ($HostSpeed -gt 1) { & $ui -GamePid $idA -Action key -Key ("0x3" + $HostSpeed) | Out-Null }
foreach ($s in $Steps) { "--- step $s"; & (Join-Path $bot $s) }
for ($r = 1; $r -le $Rounds; $r++) {
    Start-Sleep $Interval
    $out = & (Join-Path $bot "savecheck.ps1") -Tag "round$r" -Resume 2>&1
    $out | Select-String -Pattern "client:|host:|differing simulation|RESULT" | ForEach-Object { $_.Line }
    if ($out -match "RESULT: DIFFERENT") {
        $out | Select-Object -Skip 6 | Select-Object -First 70
        "first difference in round $r"; break
    }
}
