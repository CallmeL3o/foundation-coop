# Extra step for the lockstep test: the HOST opens the woodcutter's camp and places the preview, but does NOT press Bauen.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
function Step($x, $y, $wait) { & $ui -GamePid $idA -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait }
Start-Sleep 10
Step 578 712 2    # build menu
Step 580 580 2    # woodcutter's camp
Step 920 440 6    # place preview (no Bauen)
"host_preview done"
