# Foundation co-op (prototype)

Two players share **one city** in [Foundation](https://store.steampowered.com/app/690830/Foundation/)
(Polymorph Games), each on their own PC. Built with the
[Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook; the full research log is in
`games/foundation/` there.

**Status:** prototype. Tested with two game instances on one PC over the same authenticated network code two PCs
would use. Not yet tested on two real PCs. Game version: 1.12.0.16 (Steam build 26138506); other versions are
refused (coop.dll checks the code bytes it patches).

## How it works

Deterministic lockstep on top of the engine's own replay system:

- The **host** runs the simulation and records every frame like a replay (player commands, random seed, delta time),
  plus the GUIDs of objects created inside the frame and the state of two more random generators the engine uses.
  The frame is streamed to the client.
- The **client** runs the engine in replay playback mode and is fed the host's frames. Its own actions (commands,
  build-tool events) go to the host and come back in the stream.
- UI state that the simulation depends on (building previews, edit mode, part placement, preview moves, cancelling)
  is replicated as events applied at the same frame boundary on both sides, with the GUIDs of any objects they
  create.
- Simulation work the engine hands to its thread pool inside a frame (navmesh, residential lots, desirability,
  resource containers) runs inline so it finishes at the same frame on both machines.
- One TCP connection, pre-shared key, HMAC-SHA256 per message (`native/lsnet.h`). The key comes from the join
  code; the host accepts the first peer that proves the key and then locks to that IP (or SteamID).
- **Steam transport (default in the launcher):** the same authenticated byte stream goes over Steam's relay network
  (ISteamNetworkingMessages from the game's own `steam_api64.dll`): no VPN, no port forwarding. The join code then
  carries the host's SteamID instead of an IP. TCP (home network / Tailscale) remains as a fallback.
- **Lobby (v2):** the host sends its city save to the client over the same connection; when both have loaded it, the
  game starts in lockstep by itself. Player commands are held back until then.

Details, addresses and every drift source found: `games/foundation/NOTES.md` in Co-op Anything.

## Layout

| Folder | What |
|---|---|
| `native/` | `coop.dll` (C, built with Zig cc): hooks, lockstep, events, transport |
| `loader/` | v2: `FoundationCoop.exe` (launcher: game copy setup, host/join with a join code, status) and `dwmapi.dll` (proxy loader placed in the game copy: forwards all real dwmapi exports, redirects Documents/LocalAppData to the co-op profile, loads `coop.dll`; no injection) |
| `instance/` | v1 (old): `launch.exe` (starts the game suspended and injects the DLLs) and `folderhook.dll` |
| `play/` | player kit: `make_kit.ps1`, player `README.txt` (v1: `start.ps1`, `README.md`) |
| `bot/` | test harness: two instances on one PC, UI bot, `savecheck.ps1` + `savediff.py` (field-by-field save comparison) |
| `mod/`, `moddev/` | Lua test mod + dev bridge used by the harness (state fingerprints, dev console); players don't need them |
| `re/` | Ghidra headless scripts used for the reverse engineering |

## Build

Needs a portable [Zig](https://ziglang.org/) (tested 0.17.1). Set `ZIG` to your `zig.exe`:

```
bash native/build.sh      # -> native/build/coop.dll  (edit the Z= path in the script or set it to your zig)
bash loader/build.sh      # -> loader/build/FoundationCoop.exe, dwmapi.dll
bash instance/build.sh    # -> instance/build/launch.exe, folderhook.dll (v1 only)
```

`loader/gen_proxy.py` regenerates the dwmapi export forwarders from `C:\Windows\System32\dwmapi.dll`.

`play/make_kit.ps1` collects the player kit. The scripts use `E:\FoundationCoopTest` as the test folder; change it
to yours.

## Testing

`bot/run_ab_test.ps1 -RoleA host -RoleB client` starts two instances of a test copy of the game on one PC and loads
the same save. `bot/holdtest.ps1 -Step <step.ps1>` runs a scenario and compares the two games' quicksaves field by
field (`savediff.py`, ~1.9 million fields). Results so far: identical after 27 in-game days with both players
painting zones, both players building, cancelling previews, moving cameras, and through the player kit.
`bot/lobby_test.ps1` tests the v2 flow end to end (join code, save transfer, automatic start, play, savediff):
identical after 60 s and 120 s runs. `-Addr steam` uses the Steam transport; on one PC both games share one Steam
account, so only startup and relay readiness can be checked there (Steam does not connect an account to itself).

## Known issues

- A woodcutter's camp preview held in hand for minutes on the client made the client's resources age faster
  (cause not found yet).
- Untested: deleting buildings, roads, job changes, monuments with several parts, trade, quests, saving in co-op,
  two real PCs. No reconnect and no automatic resync yet.

## Safety

Only use a **copy** of the game folder; the scripts never modify the Steam install or your normal saves (co-op runs
in its own profile folder). No game files are included in this repository.
