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
| 3 | virtual Winsock for the Online lobby (the 22 functions, Netbios, DNS, SSL byte, streams, ping) | **the lobby works in the game** (login, lobby, a created game room; see below); chat and a clean quit confirmed; races with a second player: phase 4 |
| 4 | Online races (UDP 3658 between vips) against a browser player; 5-6 player games and 2-lap drift are untested on an unmodified exe | **a race between two native PCs works** (see below); a browser player, 3+ players and drift not yet |
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

1. **Phase 4, more races:** 3 or more players (a third machine, or a browser player), drift and the other race types,
   a long session, quitting right after a race; read the second machine's log too. A browser player needs the browser
   build of the repo, or th0mk's OK to use nfsu2.xyz.
2. **`server.dll`** (step 7 of phase 3): hook its imports when the game loads it, so hosting from the LAN menu works
   on the virtual LAN (today the Online menu is the way to race while the hooks are on).
3. **A second PC at home** is set up as in the phase-4 section (port proxy, firewall rule, `NFSU2_HOST`); if the
   desktop's address changes, all three need the new one (a DHCP reservation avoids it).
4. Smaller open items: fix `tools/roam_fingerprint.py` (read the names from the preprocessor); test the switch
   client's queue (`--max-peers 1`), kick/ban (`nfsu2-server ctl`), `wss://` and a claimed name; the peer bitmap
   (a connect to an absent virtual address: WSAETIMEDOUT after 3 s instead of WSAEHOSTUNREACH).

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

## Phase 4: an Online race between two PCs (passed 2026-11-06, 13:42-13:53)

- **Setup:** the desktop (192.168.0.109, the server in WSL) and a laptop (ASUS Vivobook, its own v1.2 NA install, the
  ASI loader, `out/win/dist/scripts/*`, `Server = ws://192.168.0.109:8691/lan`, name rishilaptop) on the home network.
  Server: `NFSU2_HOST=192.168.0.109` (its banner: `names: localhost, 192.168.0.109`); Windows: port proxy
  192.168.0.109:8691 -> 127.0.0.1:8691, firewall rule "NFSU2 server (LAN)" (inbound TCP 8691, LocalSubnet, Private),
  Ethernet set to Private (the player did these as administrator). `switch_probe.exe ws://192.168.0.109:8691/lan`:
  7/7 before the laptop tried. Undo: `netsh interface portproxy delete v4tov4 listenaddress=192.168.0.109
  listenport=8691; Remove-NetFirewallRule -DisplayName "NFSU2 server (LAN)"`.
- **Result (the player, photos):** the laptop joined the desktop's room; chat both ways; a race on Palomino & 16th,
  both in RX-8s: name tags over the other car, live positions (1st/2nd and the gap) on both; both showed the same
  Event Results (rishilaptop 4:04.65, rishi 4:34.65: exactly +30.00 s, the game's time limit after the first finisher;
  the server logged `1 rishilaptop 4:04.71, DNF rishi (out of time)`); both back in the Game Session Lobby with "Players Online: 2".
  Desktop at 180 fps, laptop at 60 fps: no desync seen.
- **The desktop plugin's log:** after the room, the game bound UDP 3658; at the end 5585 datagrams out, 10741 in over
  the switch, 1 "no port" (a stray while the race socket was closed), 0 queue full; TCP 0 resets, 0 refused, 0 timed
  out, 0 link resets; TX ring never full, 0 offline, 0 for an old link; ICMP 16 out, 17 in, and 1 echo from the laptop
  answered by the stack (player-to-player pings). After the race the lobby link opened again (20920, 20922). No crash
  dump.
- **Open: the desktop's lobby link went silent after the race.** Back in the lobby (new lobby connection 13:50:42),
  its TCP bytes out stopped at 5302 from 13:53:19 to 14:00 while bytes in still grew to 14925 (the server's pings
  arrived) and the game loop kept running (free roam's frame counts unchanged); the server logged `"rishi": dropped:
  silent for 125 s`. Either the game did not read the lobby socket or read it and did not answer; the log cannot
  tell (no per-connection read/unread figures yet). **The desktop game was in the background then** (the player was
  typing here): most likely the game stops servicing its lobby link while it is not the active window (it kept
  drawing frames), so the server's 125 s silence limit dropped it; the game's behaviour, not the plugin's. The
  per-connection report below shows it directly next time ("... unread for 20 s ... not reading this connection").
- **Open: the laptop at quit** (the player): "are you sure you want to quit?" at about 3 fps for a while, then 60 fps
  and a clean exit. Not a missing close from the server: its host answers the game's FIN with its own at once
  (vhost.rs on_end -> conn_end), and vsprobe checks that the stack hands that FIN to the game. **The laptop's log:**
  about 2035 frames per 30 s (68 fps) until 14:00:38; at 14:01:04 a connect to 13505 (EA Messenger), then the game's
  WSACleanup (the session ended cleanly); then 1426 frames to 14:01:13 and **48 frames from 14:01:13 to 14:02:25**
  (under 1 fps); exit 14:02:33, unloaded cleanly. The plugin's own frame step stayed small (mean 6.7 us, max 58 us)
  and free roam's thread kept talking to the hub, so not the frame hook. Guess, unproven: after the last WSACleanup
  every call fails at once (WSANOTINITIALISED, as on Windows), and a DirtySock thread at a high priority that loops on
  select spins instead of waiting, starving the laptop's render thread (the desktop has 32 hardware threads). Next:
  the same logoff and quit on the laptop with the diagnostics below.
- **Retest on the laptop with the diagnostics (14:26-14:28, lobby only, no race):** logoff (WSACleanup 14:28:20) to
  exit (14:28:23) in 3 s at about 61 fps (491 frames in 8 s): **not reproduced**. No Winsock call after the last
  WSACleanup at all (no "no session" report, nothing at exit), so nothing spun this time. Seen: the game reads its
  lobby links by polling recv (about 200 a second, 0.1 ms each) and never selects them for reading; a second thread
  at priority 2 (highest) calls recvfrom (the ICMP pings); gethostbyname runs on two threads of its own; WSAStartup
  and WSACleanup 4 each. The slow quit came after a race: next, a race, then logoff and quit.
- **Diagnostics in the plugin (2026-11-06, 331,776 bytes, asi_check PASS, in the test copy and `out/win/dist`):**
  - `[lan] calls in N s: <function> <calls> (<n> no session) <ms in it> ms (max <ms>) [<thread id>/<priority> ...]`
    every 60 s while the game calls Winsock, at the last WSACleanup ("up to the last WSACleanup"), at exit, and every
    10 s while the game calls with its session over ("with no session open"). Priority as GetThreadPriority says it
    (0 normal, 2 highest, 15 time critical). A spinning thread shows as millions of calls with "no session".
  - `[lan] virtual LAN: tcp to <vip>:<port> for N s: in/out bytes, unread, polled for reading / recv / data read
    N s ago` for every remote connection with each 60 s stats line, and once, when it happens, `... N B unread for
    20 s (polled for reading ..., recv ...): the game is not reading this connection`.
  - vsprobe (now 38 checks, all pass) checks them: an @dir answer left unread is reported after 20 s; no "no session"
    report during a session; the calls up to WSACleanup reported; a thread spinning on select after WSACleanup
    (about 97 million calls in 10 s on this PC, each failing at once with WSANOTINITIALISED) reported within 10 s.
- **Not tried yet:** a browser player in the same race (needs the browser build), 3 or more players, drift and the
  other race types, a long session, a quit right after a race; the laptop's own log was not read.

## Phase 3: virtual Winsock for the Online lobby (plan, 2026-11-06)

Verified first: SPEED2.EXE's IAT has ws2_32 at 0x7833C4..0x783418 (21 by ordinal + `WSAIoctl` by name), netapi32
`Netbios` at 0x7832CC; `*pcnfs05.ea.com` at 0x7FBDBC is in `.data` (writable). `server.dll` (EA's LAN server) imports
the same set plus getsockopt/htons/htonl/ntohs/ntohl: it is not hooked by patching the exe's IAT.

**Design.** A port of the browser's path, not a proxy: the game gets virtual sockets that live in the plugin.
- `win/online/vsock.c` (+ `vsock.h`): the virtual IP stack, ported from `runtime/web/net_lan.c` rule for rule (bind
  semantics, ephemeral ports 49152-65534, UDP unicast/broadcast/port-unreachable, the kind-4 stream protocol with
  SYN/SYNACK/DATA/FIN/RST and the connector's id, 3 s connect timeout, ICMP echo answered here, loopback and own-vip
  traffic kept local, the auto-answer rate limits). One SRW lock + one condition variable (every change wakes all
  waiters) instead of the wasm event word. The TX ring stays (in process memory, a writer thread sends to the switch
  outside the lock), so `tx_free`/`TX_RESERVE`/the control-frame queue behave exactly as in the browser.
  Transport-agnostic: frames out through a callback, frames in through `vs_rx_frame`, link state through `vs_link`.
- `win/online/vsock_ws2.c`: the stdcall Winsock surface over it, from `runtime/winsock.c`'s vlan paths: the 22
  functions, `Netbios` (NCBRESET/NCBASTAT/NCBENUM, MAC 02:4E:56:4C:x:y from the vip), `gethostbyname`
  (`pcnfs05.ea.com` -> 10.77.0.1, own name, dotted quads; everything else WSAHOST_NOT_FOUND), `gethostname`
  ("VLAN-<host number>"), `select` (sets read before written), non-blocking connect reported through select/SO_ERROR
  (never synchronously), `SIO_ROUTING_INTERFACE_QUERY` -> the vip, errors through SetLastError (WSAGetLastError is
  GetLastError on Windows too).
- `win/online/switch.c`: `sw_set_lan(on, udp ports)` sends {"t":"lan"} and puts the state into every later hello.
- Plugin (`roam_win.c`, `roam_asi.c`): with `Server =` set (and new `Online = 1`, the default), patch the exe's IAT
  entries and the SSL byte at load; the switch connection is shared with free roam and starts on the game's first
  WSAStartup if free roam has not started it; frames of kinds 3-5 go to vsock; "gone" in a peer delta resets that
  player's TCP connections (the page's NET_LAN_TCP_GONE); a reconnect resets every remote connection.
- Not in the browser and not needed natively: NetTick (real GetTickCount here), the macOS Local Network checks,
  `online.c`/`olfilter.c` (the page's persona display and Filter marking).

**Steps** (each built and checked before the next):
1. `switch.c`: `sw_set_lan` + the hello's `lan`/`udp`; probe still passes.
2. `vsock.c`: the stack, compiled freestanding.
3. `vsock_ws2.c`: the Winsock surface.
4. `vsprobe.exe` against the local server, through the replacement functions themselves: WSAStartup, DNS,
   non-blocking connect to 10.77.0.1:20920 + select, `@dir` -> ADDR/PORT and FIN, the lobby's 20922 accepts,
   ICMP echo, UDP port unreachable -> WSAECONNRESET on a connected socket, an in-process listen/accept, close semantics.
5. Plugin: IAT patch, SSL byte, the glue; `tools/asi_check.py` PASS.
6. In the game (test copy): Online -> login -> the lobby; the plugin's log and the server's.
7. Then (or phase 4): hook `server.dll` too when the game loads it (LoadLibraryA), so LAN hosting works on the
   virtual LAN instead of breaking while the hooks are on.

### Phase 3 so far (2026-11-06)

- Steps 1-5 done. `out/win/vsprobe.exe [url] [name]` (`win/online/build_probe.sh`): **all 33 checks pass** against the
  local server, through the replacement functions themselves: WSAStartup brings the link up (13 ms), names, Netbios'
  adapter, a non-blocking connect to 10.77.0.1:20920 reported writable by select, the `@dir` answer
  (ADDR=10.77.0.1 PORT=20922) then the redirector's FIN (recv 0), a refused connect in exceptfds with SO_ERROR 10061,
  a blocking connect to the lobby, getsockname/SIO_ROUTING_INTERFACE_QUERY = the vip, UDP port unreachable ->
  WSAECONNRESET, ICMP echo with the IP header, in-process listen/accept/stream/FIN, WSAEADDRINUSE, the own broadcast
  looped back, WSAEACCES without SO_BROADCAST, closesocket waking a blocked recv on another thread (WSAEINTR).
- The plugin (`NFSU2FreeRoam.asi`, 326,656 bytes, `asi_check.py` PASS) in the test copy: at load "the lobby link is
  plain TCP ... 23 imports of SPEED2.EXE redirected" (21 ws2_32 by ordinal + WSAIoctl + Netbios). `Online = 1` is in
  the .ini (repo and test copy); the phase-2 plugin and log are kept in `out/win/backup/`.
- Glue: the switch connection is shared; it starts on the game's first WSAStartup if free roam has not started it
  (name: the profile's, else `Name =`, else "Player"); the LAN state ({"t":"lan","on","udp"}) is remembered for a
  switch not started yet and goes into every hello (`sw_set_lan`); `switch.c` got `on_down`; a delta's "gone" resets
  that player's connections; free roam's stop keeps the switch while a network session is open.
- **Launching the game from the agent's background shell crashes it at start** (0xC0000005 at 0x5D2780: a call
  through the D3D9 device at [0x870974], which is null: device creation failed), *with `Online = 0` too*, so not
  phase 3; the computer-use tool cannot select the game's window either (no title, not a Start-menu app). The in-game
  test needs the player to start the game normally. PowerShell 5.1 `Set-Content -Encoding utf8` writes a BOM: edit
  the .ini with Python or an editor.
- **In the game (2026-11-06 12:45, the player started the test copy normally, all the player's mods):** logged in under
  any name, the Online Main Menu, the Game Session Lobby ("Players Online: 1"), and a created game room (Freemont, 1 lap)
  with the player in it, three green connection dots and three open slots; Start Race needs a second player. The
  plugin's log: the game's WSAStartup(2.0) took the switch free roam had already opened (10.77.29.2), then TCP to
  10.77.0.1:20920 (redirector), :20922 (lobby), :13505 (EA Messenger); ICMP 11 out, 11 answered (the dots); in 7 minutes
  0 resets, 0 refused, TX ring never full, nothing dropped; the server counted `"online":1`. Free roam's hub kept
  answering alongside. Then (the player): chat in the room showed up; Back -> Logoff -> Quit from the main menu closed the game
  cleanly. Not tried yet: a second player joining the room (phase 4).
- Not built yet: `server.dll` hooks (step 7), the peer bitmap (a connect to an absent vip times out in 3 s with
  WSAETIMEDOUT instead of WSAEHOSTUNREACH), the browser's race statistics and Filter marking.

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
