# Builds the player kit (default E:\FoundationCoopTest\kit-v2) and its zip: launcher, loader, coop.dll, README.
# No injector and no scripts: FoundationCoop.exe starts the game normally and the loader (dwmapi.dll) in the game copy
# loads coop.dll. The kit does not contain the game; each player needs their own Foundation (see README.txt).
param([string]$Out = "E:\FoundationCoopTest\kit-v2", [string]$Zip = "E:\FoundationCoopTest\foundation-coop-kit-v2.zip")
$root = Join-Path $PSScriptRoot ".."
if (Test-Path $Out) { Get-ChildItem $Out -File | Remove-Item -Force }   # files only: keeps a game copy / profiles made by testing
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item (Join-Path $root "loader\build\FoundationCoop.exe"), (Join-Path $root "loader\build\dwmapi.dll") $Out -Force
Copy-Item (Join-Path $root "native\build\coop.dll") $Out -Force
Copy-Item (Join-Path $PSScriptRoot "README.txt") $Out -Force
Copy-Item (Join-Path $root "bot\savediff.py") $Out -Force   # optional sync check for the host (needs Python)
$files = "FoundationCoop.exe", "coop.dll", "dwmapi.dll", "README.txt", "savediff.py"
$files | ForEach-Object { "{0}  {1}" -f (Get-FileHash (Join-Path $Out $_) -Algorithm SHA256).Hash, $_ } | Set-Content (Join-Path $Out "SHA256SUMS.txt")
if (Test-Path $Zip) { Remove-Item $Zip -Force }
Compress-Archive -Path ($files + "SHA256SUMS.txt" | ForEach-Object { Join-Path $Out $_ }) -DestinationPath $Zip
Get-Content (Join-Path $Out "SHA256SUMS.txt")
"zip: $Zip  $((Get-Item $Zip).Length) bytes  SHA256 $((Get-FileHash $Zip -Algorithm SHA256).Hash)"
