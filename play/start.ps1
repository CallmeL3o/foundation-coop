# Foundation co-op (prototype): start one player's game.
#   Host:   .\start.ps1 -Role host -FriendIp 192.168.1.23
#   Friend: .\start.ps1 -Role client -HostIp 192.168.1.10
# Both: load the SAME save, leave it paused, then press Enter here. The host unpauses; the friend's game follows.
param(
    [Parameter(Mandatory = $true)][ValidateSet("host", "client")][string]$Role,
    [string]$GameDir = "",          # a COPY of the Foundation folder that contains steam_appid.txt (never the Steam install)
    [string]$InstanceDir = "",      # private Documents/AppData for co-op (saves go to <InstanceDir>\Documents\Polymorph Games\Foundation\Save Game)
    [string]$HostIp = "",           # client: the host's IP address
    [string]$FriendIp = "",         # host: the only IP address allowed to connect
    [int]$Port = 27810,
    [string]$Bind = "0.0.0.0",      # host: local address to listen on (127.0.0.1 = this PC only, for testing)
    [switch]$NoPrompt               # testing: do not wait for Enter (the caller creates <InstanceDir>\lockstep_go)
)
$ErrorActionPreference = "Stop"
$kit = $PSScriptRoot
if (-not $GameDir) { $GameDir = Join-Path $kit "game" }
if (-not $InstanceDir) { $InstanceDir = Join-Path $kit "instance-$Role" }
$exe = Join-Path $GameDir "foundation.exe"
if (-not (Test-Path $exe)) { throw "foundation.exe not found in $GameDir (pass -GameDir)" }
if (-not (Test-Path (Join-Path $GameDir "steam_appid.txt"))) {
    throw "$GameDir has no steam_appid.txt. Use a copy of the game folder with a steam_appid.txt containing 690830 (do not change the Steam install)."
}
foreach ($f in "launch.exe", "folderhook.dll", "coop.dll") { if (-not (Test-Path (Join-Path $kit $f))) { throw "$f missing in $kit" } }
New-Item -ItemType Directory -Force $InstanceDir | Out-Null
Remove-Item (Join-Path $InstanceDir "lockstep_go") -ErrorAction SilentlyContinue

# shared secret: the host makes it once; the friend pastes the same 64 hex characters
$keyFile = Join-Path $InstanceDir "coop_key.txt"
if (-not (Test-Path $keyFile)) {
    if ($Role -eq "host") {
        $kb = New-Object byte[] 32; [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($kb)
        $key = ($kb | ForEach-Object { $_.ToString("x2") }) -join ""
        Set-Content $keyFile $key -NoNewline -Encoding ascii
        Write-Host "New session key (send it to your friend PRIVATELY, e.g. a direct message; anyone with it can join):"
        Write-Host $key
    } else {
        $key = (Read-Host "Paste the session key from the host (64 characters)").Trim()
        if ($key -notmatch '^[0-9a-fA-F]{64}$') { throw "that is not a 64-character key" }
        Set-Content $keyFile $key.ToLower() -NoNewline -Encoding ascii
    }
} else { Write-Host "Using the session key in $keyFile" }

$env:FCOOP_ROLE = $Role
$env:FCOOP_LS_PORT = "$Port"
if ($Role -eq "host") {
    if ($FriendIp -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { throw "pass -FriendIp <your friend's IPv4 address>" }
    $env:FCOOP_LS_BIND = $Bind
    $env:FCOOP_LS_ALLOW = $FriendIp
    Write-Host "Hosting on TCP port $Port; only $FriendIp may connect. Windows may ask to allow foundation.exe through the firewall."
} else {
    if ($HostIp -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { throw "pass -HostIp <the host's IPv4 address>" }
    $env:FCOOP_LS_HOST = $HostIp
}
$gamePid = & (Join-Path $kit "launch.exe") $exe $InstanceDir
Set-Content (Join-Path $InstanceDir "pid.txt") $gamePid
Write-Host "Game started (pid $gamePid). Saves for co-op live in $InstanceDir\Documents\Polymorph Games\Foundation\Save Game"
if ($NoPrompt) { return }
Write-Host ""
Write-Host "1. Both players load the SAME save (the host sends the .foundation file; same mods on both)."
Write-Host "2. Leave the game PAUSED."
Read-Host "3. When both games show the loaded city, press Enter"
Set-Content (Join-Path $InstanceDir "lockstep_go") "go"
if ($Role -eq "host") { Write-Host "Now unpause on the host. Log: $InstanceDir\coop.log" }
else { Write-Host "Your game now follows the host. Log: $InstanceDir\coop.log" }
