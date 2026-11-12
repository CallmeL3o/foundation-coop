# Test-only soak step for lobby_test.ps1 (-ExtraSteps): both players paint many zones (housing, logging) so the town
# grows, then the game runs at top speed for a long time. Every checkpoint: both games still alive? any desync in the
# logs? memory; then pause, quicksave both, compare with savediff.py, resume. Uses $idA/$idB/$kit/$ui/$bot of lobby_test.
param([int]$Checkpoints = 6, [int]$MinutesPer = 10)
$sg = "Documents\Polymorph Games\Foundation\Save Game"
$shots = "E:\FoundationCoopTest\shots"
New-Item -ItemType Directory -Force $shots | Out-Null
function Alive { foreach ($id in $idA, $idB) { if (-not (Get-Process -Id $id -ErrorAction SilentlyContinue)) { return $false } }; return $true }
function Paint($id, $type, $x1, $y1, $x2, $y2) {
    & $ui -GamePid $id -Action key -Key 0x08 | Out-Null; Start-Sleep 1
    & $ui -GamePid $id -Action click -X 624 -Y 712 | Out-Null; Start-Sleep 2      # zone tool
    & $ui -GamePid $id -Action click -X $type -Y 595 | Out-Null; Start-Sleep 2    # zone type (605 housing, 518 logging)
    & $ui -GamePid $id -Action drag -X $x1 -Y $y1 -X2 $x2 -Y2 $y2 | Out-Null; Start-Sleep 2
    & $ui -GamePid $id -Action drag -X $x1 -Y ($y1 + 25) -X2 $x2 -Y2 ($y2 + 25) | Out-Null; Start-Sleep 2
    & $ui -GamePid $id -Action click -X 1110 -Y 740 | Out-Null; Start-Sleep 1
    & $ui -GamePid $id -Action key -Key 0x08 | Out-Null; Start-Sleep 1
}
function Counts {
    $h = (Select-String -Path "$kit\instance-host\coop.log" -Pattern "cmds [1-9]" | Measure-Object).Count
    $c = (Select-String -Path "$kit\instance-client\coop.log" -Pattern "own command" -SimpleMatch | Measure-Object).Count
    $d = (Select-String -Path "$kit\instance-host\coop.log", "$kit\instance-client\coop.log" -Pattern "DESYNC|desync likely|desync expected" -CaseSensitive | Measure-Object).Count
    "host frames with commands: $h, client commands sent: $c, desync lines: $d"
}
function Mem { ($idA, $idB | ForEach-Object { $p = Get-Process -Id $_ -ErrorAction SilentlyContinue; if ($p) { "{0:N0} MB" -f ($p.WorkingSet64 / 1MB) } else { "gone" } }) -join " / " }

"soak: painting zones (both players)"
Start-Sleep 20
Paint $idA 605 850 400 1000 440      # host: housing on the meadow
Paint $idB 518 330 330 520 350       # client: logging in the forest
Paint $idA 605 700 350 830 370       # host: more housing
Paint $idB 605 880 470 1100 490      # client: housing
Paint $idA 518 150 250 300 270       # host: logging
Paint $idB 605 1020 360 1150 380     # client: housing
Counts
& $ui -GamePid $idA -Action key -Key 0x33 | Out-Null   # host: top speed (key 3)
"soak: running at top speed, $Checkpoints checkpoints x $MinutesPer min"
for ($cp = 1; $cp -le $Checkpoints; $cp++) {
    for ($m = 0; $m -lt $MinutesPer; $m++) {
        Start-Sleep 60
        if (-not (Alive)) { "CRASH: a game process ended during checkpoint $cp minute $m"; Counts; return }
    }
    "--- checkpoint $cp ($($cp * $MinutesPer) min): memory host/client $(Mem); $(Counts)"
    & $ui -GamePid $idA -Action shot -Out "$shots\soak-host-$cp.png" | Out-Null
    & $ui -GamePid $idB -Action shot -Out "$shots\soak-client-$cp.png" | Out-Null
    & $ui -GamePid $idA -Action click -X 216 -Y 715 | Out-Null   # host pauses
    Start-Sleep 10
    foreach ($id in $idA, $idB) { & $ui -GamePid $id -Action key -Key 0x11+0x53 | Out-Null; Start-Sleep 20 }
    $sv = @{}
    foreach ($r in "host", "client") { $sv[$r] = Get-ChildItem "$kit\instance-$r\$sg" -Recurse -Filter "quicksave-*.foundation" | Sort-Object LastWriteTime | Select-Object -Last 1 }
    "save sizes: host $([int]($sv['host'].Length / 1KB)) KB, client $([int]($sv['client'].Length / 1KB)) KB"
    python (Join-Path $bot "savediff.py") $sv['host'].FullName $sv['client'].FullName --show 10 | Select-String "differing simulation|RESULT" | ForEach-Object { "  " + $_.Line }
    if (-not (Alive)) { "CRASH: a game process ended while saving at checkpoint $cp"; return }
    & $ui -GamePid $idA -Action click -X 290 -Y 715 | Out-Null; Start-Sleep 1   # host resumes
    & $ui -GamePid $idA -Action key -Key 0x33 | Out-Null
}
"soak: done"
