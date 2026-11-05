# Foundation co-op (prototype): one city, two PCs

**Status:** early prototype. It has only been tested with two game instances on **one** PC
(over the same authenticated TCP link the two PCs would use). Expect desyncs and crashes. Back up your saves.

## How it works

The host's game runs the simulation and streams every frame (player commands, random seed, new object IDs)
to the friend's game. The friend's game replays it exactly (lockstep). The friend's own actions (building,
speed buttons) are sent to the host, which runs them for both. Your normal saves are not touched:
co-op runs in a separate profile folder (`instance-host` / `instance-client`).

## What you need (both players)

- Foundation on Steam, the **same version** (tested: 1.12.0.16, Steam build 26138506). Steam must be running.
  coop.dll checks the game code. With another version it does nothing and logs "NOT installed".
- A **copy** of the game folder, for example `D:\FoundationCoop\game` (copy `...\steamapps\common\Foundation`),
  plus a text file `steam_appid.txt` in it containing `690830`. Do not modify the Steam install itself.
- This kit folder: `launch.exe`, `folderhook.dll`, `coop.dll`, `start.ps1`. Compare the files with `SHA256SUMS.txt`.
  Antivirus or SmartScreen may complain about `launch.exe`, because it loads the DLLs into the game process.
- The same mods on both sides (best: none), and the **same save file**.

## Network

- Same home network (LAN): use the host's local IPv4 address (`ipconfig`).
- Over the internet, the safest option is a private VPN between the two PCs (for example Tailscale or ZeroTier).
  Use the VPN addresses. Port forwarding TCP 27810 to the host also works, but it exposes the port to the internet.
- The connection only accepts the friend's IP address, and it needs the shared session key. Every message is signed
  (HMAC-SHA256) and numbered. It is **not encrypted**: someone on the path could see game commands, not passwords.
- Windows will ask whether foundation.exe may accept network connections on the host. Allow it for private networks.

## Playing

1. Host: copy the save you want to play into
   `instance-host\Documents\Polymorph Games\Foundation\Save Game\` (the folder exists after the first start) and send
   the same `.foundation` file to your friend. The friend puts it into `instance-client\...\Save Game\`.
2. Host, in PowerShell inside the kit folder: `.\start.ps1 -Role host -GameDir D:\FoundationCoop\game -FriendIp <friend IP>`.
   The first time, it prints a session key. Send it to your friend privately.
3. Friend: `.\start.ps1 -Role client -GameDir D:\FoundationCoop\game -HostIp <host IP>` and paste the key.
4. Both load the same save and leave it **paused**. When both cities are loaded, press Enter in both PowerShell windows.
5. The host unpauses. From then on only the host's speed controls count. The friend's speed buttons are also sent to the host.

If PowerShell refuses to run the script, run `Set-ExecutionPolicy -Scope Process RemoteSigned` in that window first.
That setting only lasts for that window.

## Checking that both games are still the same city

Both games should stay identical. To check (optional, needs Python 3):

1. Host pauses. Wait a few seconds, then both press **Ctrl+S** (quicksave).
2. The friend sends the newest `quicksave-NN.foundation` from `instance-client\...\Save Game\<city>\` to the host.
3. Host: `python savediff.py <host quicksave> <friend quicksave>`. `RESULT: IDENTICAL` means the simulation is the same.
   UI-only fields (animations, notifications, timers) are counted separately and don't matter.

`coop.log` in each instance folder also shows `DESYNC` lines if the random state ever differs.

## What was tested (two game copies on one PC, over the same network code)

- Identical after 27 in-game days at speed x3: villagers, time controls (also from the friend), residential and
  logging zones painted by both players, houses growing in the zones.
- Identical: a woodcutter's camp built by the host and one by the friend; cancelling a building preview (X) on either
  side; the friend's camera far away from the host's; the friend holding a market preview for minutes.
- Tip: place buildings without keeping the woodcutter's camp preview in hand for many minutes. In one test that made
  the friend's resources (trees, berries, fish) age faster than the host's.
- Not tested yet: deleting buildings, roads, job assignment, monuments with several parts, trade, quests, saving and
  loading during co-op, and **two real PCs** (different CPUs or Windows versions could still behave differently).

## Known limits

- No resync: if the games drift apart, save on the host, send the save to the friend, and start both again.
- The friend's actions (and the friend's building preview following the mouse) are delayed by one network round trip.
- If the connection drops, the host's game waits (frozen) until the friend reconnects.
- Both PCs need the exact same game version and the same mods. Keep Windows updated on both PCs.
