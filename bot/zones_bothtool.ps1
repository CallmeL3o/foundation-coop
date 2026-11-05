# Scenario step: both players open (and close) the zone tool first, then only the host paints a residential zone.
$ui = Join-Path $PSScriptRoot "ui.ps1"
Start-Sleep 35
foreach ($i in "A", "B") {
    $id = [int](Get-Content "E:\FoundationCoopTest\instances\$i\pid.txt")
    & $ui -GamePid $id -Action key -Key 0x08 | Out-Null; Start-Sleep 1
    & $ui -GamePid $id -Action click -X 624 -Y 712 | Out-Null; Start-Sleep 3
    & $ui -GamePid $id -Action key -Key 0x08 | Out-Null; Start-Sleep 2
}
& (Join-Path $PSScriptRoot "zones.ps1") -HostOnly -PopupWait 0
