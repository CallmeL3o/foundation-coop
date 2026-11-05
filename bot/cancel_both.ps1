# Scenario step: host, then client open the woodcutter tool, move the preview around and cancel it (X).
param([int]$PopupWait = 35)
$ui = Join-Path $PSScriptRoot "ui.ps1"
Start-Sleep $PopupWait
foreach ($i in "A", "B") {
    $id = [int](Get-Content "E:\FoundationCoopTest\instances\$i\pid.txt")
    & $ui -GamePid $id -Action key -Key 0x08 | Out-Null; Start-Sleep 1
    & $ui -GamePid $id -Action click -X 578 -Y 712 | Out-Null; Start-Sleep 3
    & $ui -GamePid $id -Action click -X 580 -Y 580 | Out-Null; Start-Sleep 5
    foreach ($pt in @(@(700,380),@(800,420),@(880,430),@(920,440))) { & $ui -GamePid $id -Action scroll -X $pt[0] -Y $pt[1] -Key 0 | Out-Null; Start-Sleep 1 }
    & $ui -GamePid $id -Action click -X 1074 -Y 536 | Out-Null; Start-Sleep 4    # X: cancel
}
"cancel_both done"
