# Builds the player kit folder (default E:\FoundationCoopTest\kit): launcher, DLLs, start script, README.
# The kit does not contain the game; each player needs their own copy of Foundation (see README).
param([string]$Out = "E:\FoundationCoopTest\kit")
$root = Join-Path $PSScriptRoot ".."
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item "E:\FoundationCoopTest\bin\launch.exe", "E:\FoundationCoopTest\bin\folderhook.dll" $Out -Force
Copy-Item (Join-Path $root "native\build\coop.dll") $Out -Force
Copy-Item (Join-Path $PSScriptRoot "start.ps1"), (Join-Path $PSScriptRoot "README.md") $Out -Force
Copy-Item (Join-Path $root "bot\savediff.py") $Out -Force   # optional sync check (see README)
Get-ChildItem $Out -File | Where-Object Name -ne "SHA256SUMS.txt" | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name } |
    Set-Content (Join-Path $Out "SHA256SUMS.txt")
Get-Content (Join-Path $Out "SHA256SUMS.txt")
