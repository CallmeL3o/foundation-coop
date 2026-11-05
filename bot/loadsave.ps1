# Test-only: start a test instance and load the shared start save (E:\FoundationCoopTest\saves\coop-start.foundation).
# The save is copied into a fresh "Save Game\COOP" folder so it is the newest village and its only save.
# Usage: loadsave.ps1 -Instance A [-Shots]
param([string]$Instance = "A", [switch]$Shots)
$ui = Join-Path $PSScriptRoot "ui.ps1"
$inst = "E:\FoundationCoopTest\instances\$Instance"
$coop = "$inst\Documents\Polymorph Games\Foundation\Save Game\COOP"
if (Test-Path $coop) { Remove-Item -Recurse -Force $coop }
New-Item -ItemType Directory -Force $coop | Out-Null
Copy-Item "E:\FoundationCoopTest\saves\coop-start.foundation" "$coop\COOP.foundation" -Force
(Get-Item "$coop\COOP.foundation").LastWriteTime = Get-Date
(Get-Item $coop).LastWriteTime = Get-Date
$id = [int](& "E:\FoundationCoopTest\bin\launch.exe" "E:\FoundationCoopTest\game\foundation.exe" $inst)
Set-Content "$inst\pid.txt" $id
"instance $Instance pid $id"
Start-Sleep 40
function Step($x, $y, $wait, $shot) {
  & $ui -GamePid $id -Action click -X $x -Y $y | Out-Null; Start-Sleep $wait
  if ($Shots -and $shot) { & $ui -GamePid $id -Action shot -Out "E:\FoundationCoopTest\shots\load-$Instance-$shot.png" | Out-Null }
}
Step 90 542 4 "1-dialog"    # main menu: Spiel laden
Step 925 290 5 "2-after"    # newest save (COOP) at the top of the right list: Laden (saves are grouped by village id)
Step 648 413 50 "3-prompt"  # possible prompt (mods / achievements): Ja, then wait for the map to load
& $ui -GamePid $id -Action shot -Out "E:\FoundationCoopTest\shots\loaded-$Instance.png" | Out-Null
"loaded"
