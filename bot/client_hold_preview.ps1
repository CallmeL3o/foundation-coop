param([int]$ItemX = 580)
# Scenario step: the client opens the woodcutter tool and keeps the preview in hand (moving it around), never placing it.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
Start-Sleep 35
for ($i = 0; $i -lt 3 -and (Select-String -Path E:\FoundationCoopTest\instances\B\coop.log -Pattern "sent own UI event 1 " -SimpleMatch | Measure-Object).Count -eq 0; $i++) {
    & $ui -GamePid $idB -Action click -X 578 -Y 712 | Out-Null; Start-Sleep 3
    & $ui -GamePid $idB -Action click -X $ItemX -Y 580 | Out-Null; Start-Sleep 3
}
# move the cursor over the meadow (hover via a tiny drag without button would place; use scroll position moves instead)
& $ui -GamePid $idB -Action scroll -X 900 -Y 430 -Key 0 | Out-Null
"client_hold_preview done"

