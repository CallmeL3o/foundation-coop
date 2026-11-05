# Extra test step for run_ab_test.ps1: in instance B only, build a woodcutter's camp (Holzfällerlager) on the meadow,
# then screenshot both instances so A can be checked for the same building.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
function StepB($x, $y, $wait) { & $ui -GamePid $idB -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait }
StepB 578 712 2    # build menu (hammer)
StepB 580 580 2    # woodcutter's camp
StepB 920 440 3    # place preview on the meadow
StepB 840 721 8    # Bauen
StepB 1110 740 1   # close controls help
Start-Sleep 10
& $ui -GamePid $idB -Action shot -Out "E:\FoundationCoopTest\shots\82-B-lumber.png" | Out-Null
& $ui -GamePid $idA -Action shot -Out "E:\FoundationCoopTest\shots\83-A-lumber.png" | Out-Null
"b_lumber done"
