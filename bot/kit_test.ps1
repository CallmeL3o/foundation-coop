# Test-only: run the PLAYER kit (E:\FoundationCoopTest\kit, start.ps1) on this PC with two instances, without the
# Lua test mod / moddev bridge (players do not have them). Localhost only (-Bind 127.0.0.1).
param([string]$ExtraSteps = "", [int]$RunSeconds = 60)
$bot = $PSScriptRoot
Remove-Item Env:FCOOP_DEV -ErrorAction SilentlyContinue   # players run without test diagnostics
$kit = "E:\FoundationCoopTest\kit"
$ui = Join-Path $bot "ui.ps1"
$old = @(Get-Process foundation -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "E:\FoundationCoopTest\game\*" })
$old | Stop-Process -Force
foreach ($p in $old) { try { $p.WaitForExit(30000) | Out-Null } catch {} }
Start-Sleep 3
& (Join-Path $bot "..\play\make_kit.ps1") | Out-Null
if ((Get-FileHash (Join-Path $bot "..\native\build\coop.dll")).Hash -ne (Get-FileHash "$kit\coop.dll").Hash) { throw "kit coop.dll is not the fresh build" }
$aside = "E:\FoundationCoopTest\mods-aside"
foreach ($i in "A", "B") {
    $docs = "E:\FoundationCoopTest\instances\$i\Documents\Polymorph Games\Foundation"
    New-Item -ItemType Directory -Force "$aside\$i" | Out-Null
    foreach ($d in "mods\coop_probe", "moddev") { if (Test-Path "$docs\$d") { Move-Item "$docs\$d" "$aside\$i\$(Split-Path $d -Leaf)" -Force } }
    Remove-Item "E:\FoundationCoopTest\instances\$i\coop_key.txt", "E:\FoundationCoopTest\instances\$i\lockstep_go" -ErrorAction SilentlyContinue
    $coop = "$docs\Save Game\COOP"
    if (Test-Path $coop) { Remove-Item -Recurse -Force $coop }
    New-Item -ItemType Directory -Force $coop | Out-Null
    Copy-Item "E:\FoundationCoopTest\saves\coop-start.foundation" "$coop\COOP.foundation" -Force
    (Get-Item "$coop\COOP.foundation").LastWriteTime = Get-Date; (Get-Item $coop).LastWriteTime = Get-Date
}
try {
    # one instance at a time: window clicks go to whatever window is on top
    function LoadIn($i) {
        Start-Sleep 60
        $id = [int](Get-Content "E:\FoundationCoopTest\instances\$i\pid.txt")
        foreach ($s in @(@(90, 542, 4), @(925, 290, 5), @(648, 450, 50))) { & $ui -GamePid $id -Action click -X $s[0] -Y $s[1] | Out-Null; Start-Sleep $s[2] }   # load, newest save, "Trotzdem laden" (the test mod is missing on purpose)
        & $ui -GamePid $id -Action shot -Out "E:\FoundationCoopTest\shots\kit-loaded-$i.png" | Out-Null
    }
    & "$kit\start.ps1" -Role host -GameDir E:\FoundationCoopTest\game -InstanceDir E:\FoundationCoopTest\instances\A -FriendIp 127.0.0.1 -Bind 127.0.0.1 -NoPrompt
    LoadIn "A"
    # the friend pastes the host's key (here: copied)
    Copy-Item E:\FoundationCoopTest\instances\A\coop_key.txt E:\FoundationCoopTest\instances\B\coop_key.txt
    & "$kit\start.ps1" -Role client -GameDir E:\FoundationCoopTest\game -InstanceDir E:\FoundationCoopTest\instances\B -HostIp 127.0.0.1 -NoPrompt
    LoadIn "B"
    foreach ($i in "A", "B") { Set-Content "E:\FoundationCoopTest\instances\$i\lockstep_go" "go" }   # "press Enter" on both
    Start-Sleep 5
    $idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
    & $ui -GamePid $idA -Action click -X 290 -Y 715 | Out-Null   # host unpauses
    if ($ExtraSteps) { & $ExtraSteps }
    Start-Sleep $RunSeconds
    & (Join-Path $bot "savecheck.ps1") -Tag "kit" 2>&1 | Select-String "differing simulation|RESULT|ignored" | ForEach-Object { $_.Line }
} finally {
    foreach ($i in "A", "B") {
        $docs = "E:\FoundationCoopTest\instances\$i\Documents\Polymorph Games\Foundation"
        if (Test-Path "$aside\$i\coop_probe") { Move-Item "$aside\$i\coop_probe" "$docs\mods\coop_probe" -Force }
        if (Test-Path "$aside\$i\moddev") { Move-Item "$aside\$i\moddev" "$docs\moddev" -Force }
    }
}
foreach ($i in "A", "B") {
    "--- $i"
    Select-String -Path "E:\FoundationCoopTest\instances\$i\coop.log" -Pattern "NOT installed|lsnet|started|DESYNC|own command|at host seq|lockstep host: seq" -CaseSensitive |
        Select-Object -Last 8 | ForEach-Object { $_.Line }
}
