# Extra test step for run_ab_test.ps1 (roles a/b): B builds a woodcutter's camp at (920,440), A builds one at (1000,480).
# Afterwards each instance should contain both camps.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
function Step($id, $x, $y, $wait) { & $ui -GamePid $id -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait }
foreach ($p in @(@($idB, 920, 440), @($idA, 1000, 480))) {
  $id = $p[0]
  Step $id 578 712 2      # build menu
  Step $id 580 580 2      # woodcutter's camp
  Step $id $p[1] $p[2] 3  # place preview
  Step $id 840 721 8      # Bauen
  Step $id 1110 740 1     # close controls help
}
Start-Sleep 10
"ab_both done"
