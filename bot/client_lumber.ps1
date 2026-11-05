# Extra step for the lockstep test: the CLIENT (instance B) builds a woodcutter's camp; the host must get the same one.
# Uses coop.log to confirm each UI step (the rank popup and the preview pickup sometimes eat a click).
param([switch]$NoBuild, [int]$PopupWait = 35)
$ui = Join-Path $PSScriptRoot "ui.ps1"
$log = "E:\FoundationCoopTest\instances\B\coop.log"
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
function Click($x, $y, $wait) { & $ui -GamePid $idB -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait }
function Count($pat) { (Select-String -Path $log -Pattern $pat -CaseSensitive -SimpleMatch | Measure-Object).Count }
Start-Sleep $PopupWait   # let the rank popup pass
function Hover($x, $y) { & $ui -GamePid $idB -Action scroll -X $x -Y $y -Key 0 | Out-Null }
# open the woodcutter tool until the preview follows the mouse (its moves are sent to the host as event 4)
for ($try = 0; $try -lt 4; $try++) {
    $mv = Count "sent own UI event 4 "
    Click 578 712 3      # build menu
    Click 580 580 5      # woodcutter's camp (the preview part arrives after one round trip)
    foreach ($p in @(@(600, 360), @(650, 380), @(690, 395), @(700, 400))) { Hover $p[0] $p[1]; Start-Sleep 1 }
    if ((Count "sent own UI event 4 ") -gt $mv) { break }
    Click 1074 536 3     # X: close the tool and try again
}
$placed0 = (Select-String -Path $log -Pattern "setIsPlaced [0-9a-f]+ \(1,1,0\)" | Measure-Object).Count
for ($i = 0; $i -lt 4 -and (Select-String -Path $log -Pattern "setIsPlaced [0-9a-f]+ \(1,1,0\)" | Measure-Object).Count -eq $placed0; $i++) {
    Hover 705 402; Start-Sleep 1; Hover 700 400; Start-Sleep 2
    Click 700 400 4      # place preview
}
& $ui -GamePid $idB -Action shot -Out "E:\FoundationCoopTest\shots\client-preview-B.png" | Out-Null
if (-not $NoBuild) {
    Click 840 721 8      # Bauen
    Click 1110 740 1     # close controls help
}
"client_lumber done"
