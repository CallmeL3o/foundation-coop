# Extra step for the lockstep test: the HOST (instance A) builds a woodcutter's camp, then both are compared.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
function Step($x, $y, $wait) { & $ui -GamePid $idA -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait }
Start-Sleep 35   # let the rank popup pass
Step 578 712 2    # build menu
Step 580 580 2    # woodcutter's camp
Step 920 440 3    # place preview
Step 840 721 8    # Bauen
Step 1110 740 1   # close controls help
"host_lumber done"
