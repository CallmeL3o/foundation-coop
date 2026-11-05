# Scenario step: the HOST paints a residential zone on the meadow, the CLIENT paints a logging zone in the forest.
# Each paint is retried until the log shows the PaintZone command (host: in its frame stream; client: sent to host).
param([int]$PopupWait = 35, [switch]$HostOnly, [switch]$ClientOnly)
$ui = Join-Path $PSScriptRoot "ui.ps1"
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
Start-Sleep $PopupWait
function HostCmds { (Select-String -Path E:\FoundationCoopTest\instances\A\coop.log -Pattern "cmds [1-9]" | Measure-Object).Count }
function ClientZone { (Select-String -Path E:\FoundationCoopTest\instances\B\coop.log -Pattern "own command .?AVPlaCPlayerCommandPaintZone" -SimpleMatch | Measure-Object).Count }
function Paint($id, $tx, $x1, $y1, $x2, $y2, $check) {
    $before = & $check
    for ($try = 0; $try -lt 3 -and (& $check) -eq $before; $try++) {
        & $ui -GamePid $id -Action key -Key 0x08 | Out-Null; Start-Sleep 1             # close windows / popups
        & $ui -GamePid $id -Action click -X 624 -Y 712 | Out-Null; Start-Sleep 2      # zone tool ("Markieren")
        & $ui -GamePid $id -Action click -X $tx -Y 595 | Out-Null; Start-Sleep 2      # zone type
        & $ui -GamePid $id -Action drag -X $x1 -Y $y1 -X2 $x2 -Y2 $y2 | Out-Null; Start-Sleep 2
        & $ui -GamePid $id -Action drag -X $x1 -Y ($y1 + 25) -X2 $x2 -Y2 ($y2 + 25) | Out-Null; Start-Sleep 3
    }
    & $ui -GamePid $id -Action click -X 1110 -Y 740 | Out-Null; Start-Sleep 1     # close controls help
    & $ui -GamePid $id -Action key -Key 0x08 | Out-Null; Start-Sleep 1
    "paint pid $id -> " + ((& $check) - $before) + " new command record(s)"
}
if (-not $ClientOnly) { Paint $idA 605 850 400 1000 440 ${function:HostCmds} }      # host: Wohngebiet (residential)
if (-not $HostOnly) { if ($ClientOnly) { Paint $idB 605 850 400 1000 440 ${function:ClientZone} } else { Paint $idB 518 330 330 520 350 ${function:ClientZone} } }     # client: HolzfÃƒÂ¤llen (logging)
& $ui -GamePid $idA -Action shot -Out "E:\FoundationCoopTest\shots\zones-A.png" | Out-Null
& $ui -GamePid $idB -Action shot -Out "E:\FoundationCoopTest\shots\zones-B.png" | Out-Null
"zones done"

