# Lockstep check: let the game run -RunSeconds, pause the host (the client catches up to the same frame),
# then print the state fingerprint of both instances and the client's desync count.
param([int]$RunSeconds = 0, [switch]$Resume)
$ui = Join-Path $PSScriptRoot "ui.ps1"
$eval = Join-Path $PSScriptRoot "eval.py"
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
if ($Resume) { & $ui -GamePid $idA -Action click -X 216 -Y 715 | Out-Null }
Start-Sleep $RunSeconds
& $ui -GamePid $idA -Action click -X 216 -Y 715 | Out-Null   # pause (host)
Start-Sleep 5
$res = @{}
for ($try = 0; $try -lt 6; $try++) {   # the client may still be catching up to the paused frame
    foreach ($pt in 27791, 27792) {
        $r = ""
        for ($i = 0; $i -lt 3 -and -not $r.StartsWith("OK"); $i++) { $r = (python $eval --port $pt -f (Join-Path $PSScriptRoot "statehash.lua")) -join " " }
        $res[$pt] = $r
    }
    if ($res[27791] -eq $res[27792]) { break }
    Start-Sleep 5
}
foreach ($pt in 27791, 27792) { "port ${pt}: " + $res[$pt] }
"desync lines: " + (Select-String -Path E:\FoundationCoopTest\instances\B\coop.log -Pattern "DESYNC" -CaseSensitive | Measure-Object).Count
if ($res[27791] -eq $res[27792]) { "STATE MATCH" } else { "STATE DIFFERS" }
