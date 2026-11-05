FOUNDATION CO-OP (prototype) - one city, two PCs
==================================================

Fan-made test version. Expect bugs. Your normal saves are never changed: co-op uses its own
profile folders (instance-host / instance-client next to this file).

What you need (both players)
----------------------------
- Foundation on Steam, version 1.12.0.16. Steam must be running.
- This folder, unpacked (for example D:\FoundationCoop).
- About 4 GB free disk space for a copy of the game.
- A way to reach each other: the same home network, or Tailscale (free, tailscale.com) over the internet.

First time
----------
1. Double-click FoundationCoop.exe.
   Windows SmartScreen may say "Windows protected your PC" because the program is new and not
   signed: click "More info", then "Run anyway".
2. Click "Set up game copy". It copies the game from Steam into the "game" folder here
   (your Steam installation is not changed). Wait until it says "Game copy ready".

Host (shares the city)
----------------------
1. "Choose city save..." and pick the city you want to play together.
2. Pick your address (Tailscale address 100.x.x.x for the internet, or your home network).
3. "Start hosting". The game starts. Click "Copy" and send the join code to your friend
   PRIVATELY (anyone with the code can join).
   Windows may ask whether foundation.exe may use the network: allow it.
4. In the game: Load Game, pick the newest save (at the top). Keep it PAUSED.
   The status line in the launcher tells you what is happening.
5. When the status says "Playing together", unpause. Have fun!

Friend (joins)
--------------
1. Paste the join code, click "Join". The game starts and receives the host's city.
2. When the status says "Save received": Load Game, pick the newest save (at the top).
3. Wait until the status says "Playing together". The host controls the game speed.

Tips and limits
---------------
- Until both games show "Playing together", the game ignores clicks that change the city.
- Do not keep a building preview in your hand for many minutes (place it or cancel).
- Not tested yet: deleting buildings, roads, jobs, multi-part monuments, trade, quests,
  saving and loading during co-op.
- If the connection breaks, close both games and start a new session.
- If the cities look different: the host saves the game (Ctrl+S) and starts a new session with
  that save.
- Problems: click "Open co-op folder" and send coop.log (and the newest quicksave) to the host.

Files
-----
FoundationCoop.exe  the launcher
coop.dll            the co-op mod (lockstep network play)
dwmapi.dll          small loader: copied into the game copy; it loads coop.dll only when the
                    game is started from FoundationCoop.exe. Everything else is passed on to
                    the normal Windows dwmapi.dll.
SHA256SUMS.txt      checksums of the files
Source code: https://github.com/CallmeL3o/foundation-coop (MIT license)
