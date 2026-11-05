# Test-only: the new player flow on one PC. FoundationCoop.exe (command-line mode) starts host and client through the
# loader (dwmapi.dll in the game copy, no injection); the host's save goes to the client over the network; both load
# it; the lobby starts lockstep by itself; then play, pause, quicksave both and compare with savediff.py.
# Localhost only (FCOOP_LS_BIND=127.0.0.1). Profiles: <kit2>\instance-host / instance-client, seeded with settings only.
param([int]$RunSeconds = 60, [string]$ExtraSteps = "", [string]$Addr = "127.0.0.1")   # -Addr steam: Steam transport
$ErrorActionPreference = "Stop"
$bot = $PSScriptRoot
$ui = Join-Path $bot "ui.ps1"
$kit = "E:\FoundationCoopTest\kit2"
$game = "E:\FoundationCoopTest\game"
$src = Split-Path $bot
Remove-Item Env:FCOOP_DEV -ErrorAction SilentlyContinue
$env:FCOOP_LS_BIND = "127.0.0.1"
$old = @(Get-Process foundation -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$game\*" })
$old | Stop-Process -Force
foreach ($p in $old) { try { $p.WaitForExit(30000) | Out-Null } catch {} }
Start-Sleep 3
New-Item -ItemType Directory -Force $kit | Out-Null
Copy-Item "$src\loader\build\FoundationCoop.exe", "$src\loader\build\dwmapi.dll", "$src\native\build\coop.dll" $kit -Force
foreach ($r in "host", "client") {
    $inst = "$kit\instance-$r"
    if (Test-Path $inst) { Remove-Item -Recurse -Force $inst }
    $docs = "$inst\Documents\Polymorph Games\Foundation"
    New-Item -ItemType Directory -Force $docs | Out-Null
    foreach ($f in "input.config", "usersetting.config") { Copy-Item "E:\FoundationCoopTest\instances\A\Documents\Polymorph Games\Foundation\$f" $docs }
}
function NewGamePid($before) {
    for ($t = 0; $t -lt 60; $t++) {
        $p = Get-Process foundation -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$game\*" -and $before -notcontains $_.Id } | Select-Object -First 1
        if ($p) { return $p.Id }
        Start-Sleep 1
    }
    throw "game did not start"
}
function Status($r) { $f = "$kit\instance-$r\status.txt"; if (Test-Path $f) { Get-Content $f -Raw } else { "" } }
function WaitStatus($r, $pattern, $sec) {
    for ($t = 0; $t -lt $sec; $t++) { $s = Status $r; if ($s -match $pattern) { return $s }; Start-Sleep 1 }
    throw "timeout waiting for '$pattern' on $r (status: $(Status $r))"
}
function LoadNewest($id) {
    foreach ($s in @(@(90, 542, 4), @(925, 290, 5), @(648, 450, 50))) { & $ui -GamePid $id -Action click -X $s[0] -Y $s[1] | Out-Null; Start-Sleep $s[2] }
}
$before = @(Get-Process foundation -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
Remove-Item "$kit\last_code.txt" -ErrorAction SilentlyContinue
$p = [Diagnostics.Process]::Start("$kit\FoundationCoop.exe", "--host E:\FoundationCoopTest\saves\coop-start.foundation --addr $Addr --game $game"); $p.WaitForExit()   # not Start-Process -Wait: that also waits for the game
if ($p.ExitCode -ne 0) { throw "launcher --host exit $($p.ExitCode)" }
$code = Get-Content "$kit\last_code.txt" -Raw
"join code: $code"
$idA = NewGamePid $before
"host pid $idA"
Start-Sleep 90
LoadNewest $idA
"host status: $(Status host)"
$before = @(Get-Process foundation -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$p = [Diagnostics.Process]::Start("$kit\FoundationCoop.exe", "--join $($code.Trim()) --game $game"); $p.WaitForExit()
if ($p.ExitCode -ne 0) { throw "launcher --join exit $($p.ExitCode)" }
$idB = NewGamePid $before
"client pid $idB"
WaitStatus client "Save received|Now load" 180 | Out-Null
"client: $(Status client)"
Start-Sleep 45
LoadNewest $idB
WaitStatus client "^PLAY|^WARN" 120 | ForEach-Object { "client: $_" }
WaitStatus host "^PLAY|^WARN" 60 | ForEach-Object { "host: $_" }
Start-Sleep 5
& $ui -GamePid $idA -Action click -X 290 -Y 715 | Out-Null   # host unpauses
if ($ExtraSteps) { & $ExtraSteps }
Start-Sleep $RunSeconds
& $ui -GamePid $idA -Action click -X 216 -Y 715 | Out-Null   # host pauses
Start-Sleep 8
$sg = "Documents\Polymorph Games\Foundation\Save Game"
foreach ($id in $idA, $idB) { & $ui -GamePid $id -Action key -Key 0x11+0x53 | Out-Null; Start-Sleep 15 }
$sv = @{}
foreach ($r in "host", "client") {
    $sv[$r] = Get-ChildItem "$kit\instance-$r\$sg" -Recurse -Filter "quicksave-*.foundation" | Sort-Object LastWriteTime | Select-Object -Last 1
}
"saves: $($sv['host'].FullName) / $($sv['client'].FullName)"
python (Join-Path $bot "savediff.py") $sv['host'].FullName $sv['client'].FullName --show 25 | Select-String "differing simulation|RESULT|ignored" | ForEach-Object { $_.Line }
foreach ($r in "host", "client") {
    "--- $r"
    Get-Content "$kit\instance-$r\loader.log"
    Select-String -Path "$kit\instance-$r\coop.log" -Pattern "NOT installed|lsnet|lobby|status:|go \(|started|DESYNC|own command" -CaseSensitive |
        Select-Object -Last 14 | ForEach-Object { $_.Line }
}

