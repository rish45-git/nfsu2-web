# Native PC client for the browser port's online server: progress

Goal: the **original** `SPEED2.EXE` (NFSU2 PC v1.2 NA, not recompiled) plays online with the browser players of a
nfsu2-web server (nfsu2.xyz), both free roam and the game's own Online mode (lobby, races). Branch `native-online`.

Rules: no EA game file is ever copied into, committed to or uploaded from this repo (`.gitignore` blocks the usual
ones); the original `SPEED2.EXE` is never modified, not even the test copy's (everything happens in memory, from an
ASI plugin); test in `C:\Games\NFSU2-online`, never in `C:\Games\NFSU2`; the public server only after its owner
says native clients are welcome there.

## The exe

`C:\Games\NFSU2\SPEED2.EXE`: 4,800,512 bytes, SHA-256 `f9dd86c0…24168a7`, the NA v1.2 the README requires
(entry point 0x75BCC7, time stamp 0x214D4C48). The test copy's is identical.

## How the server works (what a native client must do)

- **One WebSocket** `/lan` is a virtual LAN switch (`web/server-rs/crates/lan`). Every player gets a virtual IPv4
  (10.77.0.0/18). Binary frames: `[0] kind, [4..7] src vip, [8..11] dst vip, [12..] ports + payload`, big-endian.
  Text frames are JSON: `hello` → `welcome` (vip, token), `peers`/`delta`, `ping`/`pong`, `queue`, `kick`/`ban`.
- **Online mode** = the game's own DirtySock code, unchanged, talking to the server's stand-in for EA
  (`crates/online`) at **10.77.0.1**: redirector 20920, lobby 20922, EA Messenger 13505, news/terms on 80. TCP to it
  is the switch's own stream format (kind 4: SYN/SYNACK/DATA/FIN/RST with an 8-byte LAN header,
  `runtime/web/net_proto.h`), not real TCP. Races: UDP 3658 between players' vips (kind 3), a star around the
  game's creator.
- In the browser, `runtime/winsock.c` + `runtime/web/net_lan.c` are Winsock reimplemented on that LAN. Plus two
  tricks: the string `*pcnfs05.ea.com` at **0x7FBDBC** becomes `pcnfs05.ea.com` in memory (the `*` asks for SSL 2.0
  on 20921, which the server doesn't speak) and `gethostbyname("pcnfs05.ea.com")` answers 10.77.0.1.
- **Free roam**: `runtime/roam_core.c` protocol ("N2FR" datagrams, kind 1), with the server's hub at **10.77.0.253**
  choosing each player's nearest peers. The repo's own `win/roam_asi` (NFSU2FreeRoam.asi) runs the same
  `roam_core.c`/`roam_game.c` in the original game, today over LAN UDP only.
- Login: any name (no accounts, the password is ignored); a *claimed* name needs its keyphrase's token in the hello
  (claiming is a Turnstile-protected page: the player does that in a browser). `/lan?queue` waits in line when
  full. Clients without an Origin header are allowed (`web/server-rs/src/app/lan.rs`).
- The SPEED2.EXE imports 22 ws2_32 functions by ordinal (socket, bind, connect, listen, accept, send, recv, sendto,
  recvfrom, select, ioctlsocket, setsockopt, shutdown, closesocket, getpeername, getsockname, gethostbyname,
  gethostname, WSAStartup, WSACleanup, WSAGetLastError, WSAIoctl) and netapi32 `Netbios`.

## Chosen approach

One ASI plugin (Ultimate ASI Loader, already installed) with one `wss://…/lan` connection (WinHTTP, so TLS and no
extra DLLs): hooks the game's Winsock imports and puts them on the virtual LAN as the browser does, patches the
one byte in memory, and carries free roam's datagrams (roam_core with the hub). No admin rights, no hosts file, no
driver, no file of the game changed. (A proxy + hosts file alone cannot work: races go to 10.77.x.x addresses the
PC cannot reach without a virtual network adapter, and the SSL byte needs a patch anyway.)

| Phase | What | State |
|---|---|---|
| 0 | branch, .gitignore, test copy, toolchain, the existing free-roam ASI built and tried, local server | **done** |
| 1 | switch client in C (WebSocket, hello/welcome, ping, queue, kick), a console probe against the local server | **done** (see below) |
| 2 | free roam online: a switch transport for the ASI, the hub | **done** against simulated players (see below); not yet with a browser player; the exe-fingerprint generator is still unfixed |
| 3 | virtual Winsock for the Online lobby (the 22 functions, Netbios, DNS, SSL byte, streams, ping) | the bulk |
| 4 | Online races (UDP 3658 between vips) against a browser player; 5-6 player games and 2-lap drift are untested on an unmodified exe | |
| 5 | claim token, idle release, logs; nfsu2.xyz only with the owner's OK | |

## Environment (this PC)

- Windows 11, Ryzen 9 9950X3D, RX 9070 XT, desktop 2560×1440 @ 180 Hz.
- MSYS2 in `C:\msys64`, 32-bit GCC 16.2 in `C:\msys64\mingw32\bin` (`pacman -S mingw-w64-i686-gcc`).
- Python venv in `.venv` (`requirements.txt`). On Windows run the tools as
  `.venv/Scripts/python -I -X utf8 tools/<tool>.py` (without `-X utf8` they read UTF-8 sources as cp1252; `-I`
  ignores `PYTHONUTF8`).
- The Rust server builds on Linux/macOS only (Unix-only calls from its first crate on): it runs in WSL 2 with
  `win/online/wsl-server.sh` (`setup`, `build`, `run`, `bots`). It needs only `--data` (a folder with SPEED2.EXE),
  not the page or the wasm module, and listens on http://127.0.0.1:8080. `bots` runs the repo's `nfsu2-roamload`
  (simulated free-roam players), so free roam can be tested without building the browser version.

## Build the free-roam ASI

```
PATH="/c/msys64/mingw32/bin:$PATH" bash tools/build_roam_asi.sh
```

Output in `out/win/` (ignored by git). The script's last line (`stat -f %z`, macOS) fails in Git Bash after the build
has finished: harmless. Check: `.venv/Scripts/python -I -X utf8 tools/asi_check.py out/win/NFSU2FreeRoam.asi
/c/Games/NFSU2-online/SPEED2.EXE` → PASS.

Fixes this needed (upstream's Windows build had fallen behind the shared code):
- `win/roam_asi/roam_host_win.h` includes `splitscreen_mem.h` (`SPLIT_PLAYERS_INDEX`, 0x8900AC on the real exe);
- `runtime/roam_game.c`: the flag pointers in `tags_update_for` only `#if FLAGS` (off on Windows); a Windows
  `g_cp_n_get()` returning 0 (the real one is in the port-only tail, no co-op on Windows);
- `win/roam_asi/roam_win.c`, `roam_asi.c`: `#undef far` / `near` after `windows.h` (roam_core.h has fields so named);
- `win/roam_asi/roam_libc.c`: `strcpy`, `strcat` (GCC 16 emits calls to them; the plugin has no C runtime).

Known: `tools/roam_fingerprint.py` stops at `G_VIEW2` (it reads every `G_`/`FN_` of roam_game.c, port-only ones too).
The committed `roam_fp.h` covers 117 of the 119 addresses the Windows build uses (not `G_CAREER_RACE`, `G_ENGAGE`).
Fix in phase 2: take the names from the preprocessor (`gcc -E -dM -DROAM_HOST_WIN`).

## Test runs

**2026-11-06, test copy, all the player's mods, NFSU2FreeRoam.asi (LAN only):** 60 fps, a race and free roam.
- The plugin's exe check passed (83 functions, 1 replaced by another mod: fine; 128 global uses), the frame hook at
  0x581475 went in after ExtraOptions' (0x581470), free roam on, no fault in its log.
- A crash at 11:01:33 after changing the resolution in-game and alt-tabbing: Windows recorded it in
  **NFSU2Unlimiter.asi** 3.1.1 (+0x1482E, 0xC0000409, a fail-fast abort), then a jump to a garbage address while
  the process was going down. The crashing thread's stack has Unlimiter, game and AMD D3D9 driver (amdxn32.dll)
  frames and **no NFSU2FreeRoam.asi address**: not the plugin. The game was already borderless (WidescreenFix
  WindowedMode = 4), so not the exclusive-fullscreen alt-tab crash: the device reset after the resolution change.
  The plugin's log of that run was overwritten by the next start (it opens with CREATE_ALWAYS).
- Changed in the test copy: WidescreenFix `ResX = 2560`, `ResY = 1440` (were 0), still borderless fullscreen.
  Don't change the resolution in the game's menu.
- Seen: ReShade is installed twice (`d3d9.dll` and `scripts\ReShade.asi`); the second aborts itself ("Another
  ReShade instance was already loaded"): harmless, but `scripts\ReShade.asi` can go.

## Mods and online

Fine: ASI loader (needed), ReShade, WidescreenFix, ExtraOptions, HD reflections, SpeedReflect, TexWizard, Trax
renamer, sound/animation mods, `server.dll`/`server.cfg` (EA's LAN server, the game's own). Online risks:
**Unlimiter** (cars and parts browser players don't have; also the crash above) and **StanceMod** (only you see the
stance): drive stock cars online, or disable them in the test copy.

## Next

1. **Phase 3**, the game's own Online lobby (not started; wait for the go-ahead): redirect the exe's 22 ws2_32
   imports (by ordinal) and `Netbios` through the switch client; `gethostbyname("pcnfs05.ea.com")` -> 10.77.0.1;
   the `*pcnfs05.ea.com` string at 0x7FBDBC -> `pcnfs05.ea.com`; TCP as the switch's stream frames (kind 4), UDP as
   kind 3, ICMP echo as kind 5. `runtime/winsock.c` and `runtime/web/net_lan.c` describe the behaviour exactly.
2. A test with a real browser player (needs the browser build of the repo, or th0mk's OK to use nfsu2.xyz).
3. Smaller open items: fix `tools/roam_fingerprint.py` (read the names from the preprocessor); test the switch
   client's queue (`--max-peers 1`), kick/ban (`nfsu2-server ctl`), `wss://` and a claimed name; the build script's
   last line uses macOS `stat -f`.

## Local server (verified 2026-11-06)

- Built in WSL: `~/.cache/nfsu2-native/target/release/nfsu2-server` and `nfsu2-roamload`.
- **Port 8080 is taken on this PC by Steam's web helper** (`steamwebhelper` listens on 127.0.0.1:8080, and answers
  404 to everything): use `NFSU2_HTTP_PORT=8691`. **Keep a `wsl.exe` session open for as long as it runs** (a process
  started with nohup/setsid is killed when the last wsl.exe session ends). From Git Bash, as a background task:
  `MSYS_NO_PATHCONV=1 wsl.exe -e bash -lc 'NFSU2_HTTP_PORT=8691 exec bash /mnt/c/dev/nfsu2-web/win/online/wsl-server.sh run'`
- Windows reaches it at `http://127.0.0.1:8691/` (WSL localhost forwarding works, no mirrored mode needed);
  `/players` answers `{"players":0,...}`; the switch is `ws://127.0.0.1:8691/lan`.
- `wsl-server.sh bots 3 15` (3 simulated roam players, 15 s): all 3 welcomed and linked, 2349 snapshots checked, 0 bad,
  p50 latency 27 ms, hub and switch work. The server log shows each join/leave as `[lan] 10.77.x.2 joined`.
- Startup notes: Online (10.77.0.1) and the free-roam hub (10.77.0.253) are on; `game.wasm` is missing (not needed).

## Phase 2: free roam online (done 2026-11-06, with simulated players)

- **How:** `NFSU2FreeRoam.ini`: `Server = ws://127.0.0.1:8691/lan` (`wss://host/lan` for HTTPS). `roam_win.c` then opens
  no UDP socket: datagrams go through `win/online/switch.c` as kind-1 frames, the server's hub (10.77.0.253) picks the
  players near us, one SNAP state per update, a thread ticks the session. The switch starts when the profile's name is
  known (45 s at the latest). `Claim =` takes a claimed name's token. `Collide = 0` stays (ghost cars: you and friends
  drive through each other; the shipped default). `DumpCar = 1` saves the car's record to `scripts\NFSU2FreeRoam.car`.
  Needs Windows 8 or newer (WinHTTP WebSocket); `tools/asi_check.py` allows WINHTTP, four heap functions, < 384 KB.
- **Results (test copy, all the player's mods):** joined the switch as 10.77.x.2 under the profile name, the hub
  answered, 0 send errors. With 3 real-core bots the game logged `JOIN: RoamBot1/2/3 joined`, `3 friend(s)`, the
  cars appeared (the screenshots: friend cars drive in the player's world, orange Players markers on the minimap,
  the world map's markers drew), and `LEAVE ... car removed` when the bots stopped. The 3 bots saw each other and
  the game too (`bad 0`).
- **Fixed on the way:**
  - Quitting the game crashed in `webio.dll` (WinHTTP, 0xC0000409): the plugin tried to close the WebSocket in
    `DLL_PROCESS_DETACH`, when every other thread is already gone. Now `roam_win_exiting` (set in DllMain when
    `reserved`) skips every WinHTTP call at exit (the server drops the player a few seconds later, as for a closed
    tab). The run after it exited cleanly (no dump, "unloaded (game exit)").
  - The core logs the first 8 refused datagrams (error, type, length, first bytes): that found the bots' problem.
- **Lessons about the test tools (not plugin bugs):**
  - The hub links and lists players of the **same room** only: `roamload` defaulted to room 7, the plugin has room 0
    (empty `Room =`). `wsl-server.sh bots` now uses room 0 (`NFSU2_BOT_ROOM`).
  - It links by **distance** too: the bots were 1.9 km away at the world origin. `NFSU2_BOT_CENTER="x,y,z"`; the
    career start is (1695.3, -883.6, 24.1); the player's position can be read from the running game (read only):
    `Player* = [0x8900AC]`, car = `[Player+4]`, position = floats at `car + 0x60` (wheel heights `car + 0x130 + 0x40*w + 0x38`).
  - `nfsu2-roamload` is a **load generator**: its car states are not the protocol's (orientation as 4 floats, the
    core reads 4 int16: "error 9, out of range"), so no real game ever accepts them. Use `out/win/roambot.exe`
    (`win/online/roambot.c`, built by `win/online/build_probe.sh`): the real `roam_core.c` + `switch.c`.
    `roambot.exe ws://127.0.0.1:8691/lan synth 3 <x> <y> <z> <seconds> "" [radius=8]`; `synth` is a made-up 350Z
    record (the game accepts it), or the path of a `.car` file. Its wheel heights are +0.28 (a real car's 0.26 to
    0.29), and it drives a small slow circle: the bots know no walls and one height (a 30 m circle went through the
    plaza's walls and sank into its ground).
- **Windows crash dumps:** `%LOCALAPPDATA%\CrashDumps\SPEED2.EXE*.dmp` (the first, 11:01, was Unlimiter's); the
  minidump Python module reads them (`pip install minidump` in the venv).
- **Not tested:** a browser player, the world map's markers for far players with real hub answers, a long session,
  several real games at once, more than 3 friends, races, the Online lobby.

## Phase 1: the switch client (done 2026-11-06)

- `win/online/switch.h`, `switch.c`: WinHTTP WebSocket client of `/lan`, KERNEL32 + WINHTTP only, compiled with the
  plugin's freestanding flags (it can go into the ASI as it is). A reader thread runs the callbacks; a second thread
  pings every `ping_ms`. Sends a plain hello (`name`, `lan`, optional `claim`): no bundle/compact/delta, so the server
  sends and takes one frame per message. Reconnects by itself (250 ms doubling to 5 s) with `?queue`, and with
  `?resume` + the old vip and token in the hello. Kick, ban and rename (messages or close codes 4001/4003/4004)
  end it for good (`SW_REMOVED`). Close 1013 (full) just retries.
- `win/online/probe.c`, `build_probe.sh`: `PATH="/c/msys64/mingw32/bin:$PATH" bash win/online/build_probe.sh` makes
  `out/win/switch_probe.exe [url] [name] [seconds]`. Against the local server all 7 checks pass: welcome (the vip
  and a 22-char token), the peer list with "NFSU2 Online" (`server:true`, 10.77.0.1), an ICMP echo answered, a TCP SYN
  to 20920 answered with SYNACK, a SYN to a closed port with RST, UDP to 10.77.0.1 with port-unreachable, ping/pong
  (0.4 to 0.6 ms). Restarting the server mid-run: the client retried (WinHTTP error 12029 = refused) and came back
  with the same address (`[lan] 10.77.0.2 is back`).
- Not tested yet: `queue` (needs a full server: `--max-peers 1`), kick/ban (`nfsu2-server ctl`), `wss://`, a claimed
  name, bundles (not used).
- Frame layout for the LAN kinds, as the probe sends it: 16-byte header (kind, 0, 0, 0, src vip, dst vip, sport, dport),
  then the 8-byte LAN header (op, ttl, 0, 0, connection id little-endian), then data. ICMP/echo payload starts with the
  ICMP type (8 request, 0 reply). Replies come with the server's vip as the source.
