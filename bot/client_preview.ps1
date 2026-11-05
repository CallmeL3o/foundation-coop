# Extra step for the lockstep test: the CLIENT (instance B) builds a woodcutter's camp; the host must get the same one.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
function Step($x, $y, $wait) { & $ui -GamePid $idB -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait }
Start-Sleep 20     # let the rank popup pass
Step 578 712 3    # build menu
Step 580 580 3    # woodcutter's camp
Step 920 440 4    # place preview
& $ui -GamePid $idB -Action shot -Out "E:\FoundationCoopTest\shots\client-preview-B.png" | Out-Null


"client_preview done"
