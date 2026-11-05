# Strong lockstep check: pause the host, let the client reach the same frame (Lua fingerprints equal), quicksave both
# (Ctrl+S) and compare the two saves field by field with savediff.py. Leaves the game paused unless -Resume.
param([string]$Tag = "check", [switch]$Resume, [string]$Ignore = "")
$bot = $PSScriptRoot
$ui = Join-Path $bot "ui.ps1"
$idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
$idB = [int](Get-Content "E:\FoundationCoopTest\instances\B\pid.txt")
& $ui -GamePid $idA -Action click -X 216 -Y 715 | Out-Null   # pause (host)
Start-Sleep 5
$fa = $fb = ""
for ($t = 0; $t -lt 8; $t++) {
    $fa = (python (Join-Path $bot "eval.py") --port 27791 -f (Join-Path $bot "statehash.lua")) -join " "
    $fb = (python (Join-Path $bot "eval.py") --port 27792 -f (Join-Path $bot "statehash.lua")) -join " "
    if ($fa -eq $fb -and $fa.StartsWith("OK")) { break }
    Start-Sleep 4
}
"[$Tag] host:   $fa"
"[$Tag] client: $fb"
$sg = "Documents\Polymorph Games\Foundation\Save Game"
$before = @{}
foreach ($i in "A", "B") { $before[$i] = (Get-ChildItem "E:\FoundationCoopTest\instances\$i\$sg" -Recurse -Filter "quicksave-*.foundation" | Measure-Object).Count }
foreach ($id in $idA, $idB) { & $ui -GamePid $id -Action key -Key 0x11+0x53 | Out-Null; Start-Sleep 15 }
$sv = @{}
foreach ($i in "A", "B") {
    $sv[$i] = Get-ChildItem "E:\FoundationCoopTest\instances\$i\$sg" -Recurse -Filter "quicksave-*.foundation" | Sort-Object LastWriteTime | Select-Object -Last 1
}
"[$Tag] saves: $($sv['A'].Name) / $($sv['B'].Name)"
$args2 = @($sv['A'].FullName, $sv['B'].FullName, "--show", "25")
if ($Ignore) { $args2 += @("--ignore", $Ignore) }
python (Join-Path $bot "savediff.py") @args2
if ($fa -eq $fb) { "[$Tag] FINGERPRINT MATCH" } else { "[$Tag] FINGERPRINT DIFFERS" }
if ($Resume) { & $ui -GamePid $idA -Action click -X 216 -Y 715 | Out-Null }
