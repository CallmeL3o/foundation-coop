# Test-only: run the two-instance command transport test on this PC (localhost only, test copy only).
# Closes running test instances, installs the freshly built coop.dll into E:\FoundationCoopTest\bin,
# starts instance A (FCOOP_ROLE=recv) and B (FCOOP_ROLE=send) on the same map code, then prints both logs.
# Usage: .\foundation-coop\bot\run_ab_test.ps1 [-MapCode <code>] [-ExtraSteps <path to a .ps1 run against B after setup>]
param([string]$MapCode = "52S1rS1WT6", [string]$ExtraSteps = "", [int]$WaitAfter = 10, [string]$RoleA = "recv", [string]$RoleB = "send", [switch]$WrongKey)
$ErrorActionPreference = "Continue"
$env:FCOOP_DEV = "1"   # test diagnostics in coop.dll (creation log, lot dumps); the player kit leaves this off
$bot = $PSScriptRoot
$built = Join-Path $PSScriptRoot "..\native\build\coop.dll"
$old = @(Get-Process foundation -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "E:\FoundationCoopTest\game\*" })
$old | Stop-Process -Force
foreach ($p in $old) { try { $p.WaitForExit(30000) | Out-Null } catch {} }
# install the fresh DLL; the old processes may hold the file for a moment after exiting
$copied = $false
for ($t = 0; $t -lt 30 -and -not $copied; $t++) {
    try { Copy-Item $built "E:\FoundationCoopTest\bin\coop.dll" -Force -ErrorAction Stop; $copied = $true } catch { Start-Sleep 1 }
}
if (-not $copied -or (Get-FileHash $built).Hash -ne (Get-FileHash "E:\FoundationCoopTest\bin\coop.dll").Hash) { throw "could not install the new coop.dll" }
# install the current Lua mod and moddev bridge into both test instances
foreach ($inst in "A", "B") {
  $docs = "E:\FoundationCoopTest\instances\$inst\Documents\Polymorph Games\Foundation"
  Copy-Item (Join-Path $PSScriptRoot "..\mod\coop_probe\*") "$docs\mods\coop_probe\" -Force
  Copy-Item (Join-Path $PSScriptRoot "..\moddev\coop_bridge.lua") "$docs\moddev\coop_bridge.lua" -Force
}
if ($RoleA -eq "host") {
  # lockstep test: both load the shared start save (paused), then the host is unpaused; the client must follow
  foreach ($i in "A", "B") { Remove-Item "E:\FoundationCoopTest\instances\$i\lockstep_go" -ErrorAction SilentlyContinue }
  # a fresh shared session key for this test run (lsnet.h: 32 random bytes as 64 hex chars); -WrongKey tests rejection
  $kb = New-Object byte[] 32; [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($kb)
  $key = ($kb | ForEach-Object { $_.ToString("x2") }) -join ""
  Set-Content "E:\FoundationCoopTest\instances\A\coop_key.txt" $key -NoNewline -Encoding ascii
  if ($WrongKey) { $kb[0] = $kb[0] -bxor 1; $key = ($kb | ForEach-Object { $_.ToString("x2") }) -join "" }
  Set-Content "E:\FoundationCoopTest\instances\B\coop_key.txt" $key -NoNewline -Encoding ascii
  $env:FCOOP_ROLE = $RoleA; & (Join-Path $bot "loadsave.ps1") -Instance A | Out-Null
  $env:FCOOP_ROLE = $RoleB; & (Join-Path $bot "loadsave.ps1") -Instance B | Out-Null
  $env:FCOOP_ROLE = ""
  foreach ($i in "A", "B") { Set-Content "E:\FoundationCoopTest\instances\$i\lockstep_go" "go" }
  Start-Sleep 5
  $idA = [int](Get-Content "E:\FoundationCoopTest\instances\A\pid.txt")
  & (Join-Path $bot "ui.ps1") -GamePid $idA -Action click -X 290 -Y 715 | Out-Null   # host: speed up from paused
} else {
  $env:FCOOP_ROLE = $RoleA
  & (Join-Path $bot "newgame.ps1") -Instance A -MapCode $MapCode | Out-Null
  $env:FCOOP_ROLE = $RoleB
  try { & (Join-Path $bot "newgame.ps1") -Instance B -MapCode $MapCode | Out-Null } catch { "B bot error: $_" }
  $env:FCOOP_ROLE = ""
}
if ($ExtraSteps) { try { & $ExtraSteps } catch { "extra steps error: $_" } }
Start-Sleep $WaitAfter
"--- B (send)"
Get-Content "E:\FoundationCoopTest\instances\B\coop.log" -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch "seed_after" } | Select-Object -First 30
"--- A (recv)"
Get-Content "E:\FoundationCoopTest\instances\A\coop.log" -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch "seed_after" } | Select-Object -First 40
"--- running test instances"
Get-Process foundation -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "E:\FoundationCoopTest\game\*" } | Select-Object Id | Format-Table -HideTableHeaders | Out-String
