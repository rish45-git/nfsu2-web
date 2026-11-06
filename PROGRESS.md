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
| 6 | free-roam race lobbies (roam_game.c `mp_*` + mprace.c) in the plugin, for two players (host + one friend) on our own server; a runtime server switch | **steps 1-5 built and committed** (below); checked by probes against the local server, **nothing of it tried in the game yet** |

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

## Phase 6: free-roam race lobbies in the plugin (plan, 2026-11-06)

**How the browser does it (read for this plan).** Two halves, both shared C:
- *The lobby*, `runtime/roam_game.c` `mp_*` (behind `MPRACES`, 0 when `ROAM_HOST_WIN`): race spots from the career's
  event triggers (`g_spot`); the race card's Accept asks Solo / Multiplayer (`mp_card`, a patch of the card's handler
  0x499C00, the game's own two-button dialog). The host's lobby sends `RC_MSG_LOBBY` ADVERT every second to its peers
  and to the hub (world map, `LOBBIES`); JOIN / LEAVE to the host, START / CLOSE from it. Several lobbies can sit on one
  spot (the card pages through them: next E, new B, who-can-join V). Players join within 15 m and stay within 40 m;
  the hub links up to 24 players within 20 m of a start to its host first. Enter with 2+ in it: 2 s countdown, START
  names who races, and every game starts its trip. The protocol (`roam_core.c`) is already in the plugin; only the
  game side is off.
- *The trip*, `runtime/mprace.c`: each game drives itself through screens a player could use: pause, Quit, Main, LAN;
  the host starts **server.dll** early (StartLANServer 0x5FA300) while the lobby counts down, creates a passworded game
  on the race's track; joiners connect straight to the host's vip:9900, find the game by its host's LAN name, gjoi,
  Ready; the host starts once all are ready (30 s at most). Then the results, LAN exit, Resume Career, the crib (the
  prize into the bank), Return to Explore Mode: back at the spot. Also: engine-power balancing (logf/expf/powf, the
  same numbers in every game), the career credit for a win (`mpcredit.c`), URL heats, the quit fee and "N2QT" quit
  datagrams on a UDP port (`racehorn.c`, `vlan_rt_send`).
- *Invites today*: only from the page (`/friends` text messages on `/lan`, `friends.rs`), only between claimed-name
  friends both ways; the host takes a JOIN from afar only from a friend or crew-mate (`mp_recv` RC_LOBBY_JOIN).

**Scope (narrowed 2026-11-06):** two players, the player hosting and one friend joining, on our own server
(`NFSU2_HOST=192.168.0.109`), free roam and career races together. No open-invite broadcast, no multi-lobby UI, no
hub changes, no live server. Nothing of phases 1-4 changes. (The game's own limit, 6 cars a race, is untouched: the
shared code still takes more, we just do not build or test for it.)

**What the plugin lacks for it.**
1. **server.dll on the virtual LAN** (the host's half; PROGRESS "Next" 2): the game LoadLibrary's it; its own ws2_32
   imports (plus getsockopt, htons/htonl/ntohs/ntohl) must go to vsock too. Hook the exe's LoadLibraryA/FreeLibrary
   imports, patch server.dll's IAT on load. Its LAN broadcast (the "server found" message) on vsock's broadcast path.
2. **mprace.c's host layer**: it is written against the emulator (`call`/`dispatch_indirect`, `dispatch_patch` of
   two game handlers: the pause menu 0x4C2170 and the race card 0x499C00, `autotest_top_fng`/`career_fe_active`,
   `autotest_tap` for about a dozen Enter/Esc/Left presses, `mem_is_committed`, stdio, getenv, clock_gettime, libm).
   A `mprace_host_win.h` as `roam_host_win.h` is for roam_game.c: calls through `roam_win_call`, the FE manager's
   screen list (0x8637B4) for the top screen, native detours (jmp + trampoline) for the two handlers, a DirectInput8
   keyboard hook (GetDeviceState) for the taps and for muting the player's input during the trip (`mprace_owns_input`).
3. **Deterministic libm**: the balance must give every game of a race the same factor, browser or native. Port the
   browser build's own logf/expf/powf (same algorithm, bit for bit) into roam_libc.c; a test vector checks both.
4. `mpcredit.c` (career credit), the parts of `racenet.c` the trip reads (`racenet_jump`, race peers), N2QT over vsock.
5. **UI without a page**: `roam_plat_lobby` returns 0, so `mp_line` puts the lobby on free roam's top line (already
   written for that case); the card and dialogs are the game's own; `roam_plat_lobby_act` from plugin hotkeys
   (next / new / who-can-join / leave / join an invite). Labels over spots and map icons after the trip works.

**Two-player invite and join (no protocol or hub change; the browser's behaviour stays as it is).**
- *Who is "us"*: both games set the same private `Room =` in the .ini. The hub links and lists players of the same room
  only, so the room is the pair: no friends list, claimed name or Turnstile needed (the friends system lives on the
  page's `/friends` messages, which the plugin has no page for). With two players in a room the hub should link them
  wherever they are (it chooses by distance once a room has more players than links); not checked yet: step 4 checks
  that an ADVERT reaches the friend 2 km away.
- *Drive up* (exists): the host opens a lobby with the race card's Multiplayer; the friend drives to the marker (15 m)
  and Enter joins; the host's Enter starts it.
- *Invite from afar* (small): the host's ADVERTs already reach the friend directly (they are linked). The friend's top
  line says "rishi hosts a sprint at the marker: press J to join"; J is `mp_far_join` (exists). The host takes a JOIN
  from afar today only from a friend or crew-mate (`mp_recv` RC_LOBBY_JOIN): in the plugin (`ROAM_HOST_WIN`, option
  `FarJoin = 1`) it also takes one from anyone in its room. The trip leaves from where the friend is and brings it back
  there (as now).
- *Career credit* ("co-op" as far as the game allows): each player drives its own career in its own game; a race counts
  in a career when `mp_counts` says so in that game (a race of the player's stage, open, not won yet). A win by the
  other player counts for us only when the hub marks us friends (`MpRace.friends`), which needs the page.
- *Partner (decided 2026-11-06: yes)*: plugin option `Partner = <name>`, the name over the other player's car (its
  profile name, or its `Name =`). In `roam_game.c` `mp_friend(id)`, under `ROAM_HOST_WIN` only: a peer whose name is the
  Partner (case-insensitive) is a friend, as the existing `NFSU2_MPRACE_TEST_FRIENDS` test list makes one. From there
  the existing code does the rest:
  - `mp_trip_names` puts it in `friend_mask`, and `mp_trip` passes its LAN name in `MpRace.friends`;
  - mprace.c `credit_decide`: when the winner is a friend and we finished, `MPC_FRIEND`, and `mpcredit_win` gives our
    career the game's own win of that race (with the winner's margin), if the race counts in *our* career (`t.credit`,
    our game's `mp_counts`). Otherwise only the place's prize money, as now;
  - it is also a "mate" for the lobby: a JOIN from afar taken, our stage limit waived for it, and it gets into a
    "Friends & crew only" lobby.
  - Each side sets the other's name (desktop `Partner = rishilaptop`, laptop `Partner = rishi`); one side alone only
    credits that side. A name is no proof of who drives (anyone can play under any unclaimed name), which is why it
    goes with a private `Room`: only the two of us are there. Log: `[roam] Partner "<name>" ... a friend for race
    credit`, and mprace's existing "our friend won it: it counts" line.
- Not built: open invites, the card's paging between lobbies, more than one lobby at a spot (the shared code keeps
  doing what it does; we only test with two).

**Server switch at runtime (new, plugin only).** Ready for the live server later, not connected to it now.
- .ini: `Server =` stays the server at start. New `Servers = local=ws://192.168.0.109:8691/lan` (a list of
  `name=url`, `;`-separated, each optionally `|claim token`, since a claim token is one server's). The shipped .ini has
  the local entry and a commented-out example only: nothing reaches nfsu2.xyz unless the player adds it.
- Hotkey `ServerKey = F8` (default): the next entry. Only in free roam proper: refused (top line says why) in a race,
  the LAN or Online menus, a race trip or a lobby, or while a Winsock session of the game is open (`vs_session_active`),
  since a swap drops every virtual connection.
- On a worker thread (sw_stop waits up to 2 s): `sw_stop` the old switch; `sw_start` the new one with no resume token
  (a token is one server's); vsock gets link down, then link up with the new vip (which already resets every remote
  connection); free roam's session drops its peers and says HELLO to the new hub. The top line: "Server: live
  (wss://nfsu2.xyz/lan): connecting", then "... connected as 10.77.x.y" or the switch's own error (refused, HTTP 403,
  banned ...); F8 again goes on to the next entry (back to local).
- Only between servers: the plugin must start with `Server =` set (a switch from LAN UDP to a server is not in scope).
  The choice is not written back to the .ini (the next start is on `Server =` again).
- Input: the game's DirectInput keyboard (a vtable hook of its device's GetDeviceState: no new import for
  `asi_check.py`, and keys only while the game has focus). The same hook does mprace's key presses and input muting.

**Done so far (2026-11-06, while the player slept: built and checked here; nothing of it tried in the game yet):**
- Step 1 (commit "Phase 6 step 1"): `win/roam_asi/roam_keys.c` (hotkeys by GetAsyncKeyState while the game is in
  front, user32 looked up with GetProcAddress, the one new import, allowed in `asi_check.py`; the game's keyboard
  [0x86E5C0] hooked: GetDeviceData / SetActionMap vtable slots, taps through its action map, the player's events held
  back while muted, our events still delivered while the device is lost); a HUD hook at 0x6005E0 (the browser's site:
  `pop edi/esi/ebx; ret` + nop replaced by a jmp, its own fault way back), which now runs `roam_game_hud` natively:
  the line at the top of the HUD **and the names over other players' cars** (`Labels = 0` turns those off); the server
  switch (`Servers =`, `ServerKey = F8`, `roam_win.c`), `rc_new_server` in the core (the old server's peers dropped, a
  HELLO to the new hub), `roam_game_note` / `roam_game_switch_block` in roam_game.c (Windows only).
  `out/win/swaptest.exe` (`win/online/swaptest.c`, built by `build_probe.sh`: the plugin's own roam_win.c, switch and
  core, with stand-ins for the game) against the local server with a roambot: **all 17 checks pass** (refused while
  blocked, refused while switching, a new vip on each server, the old server's player dropped, the bot linked again
  on the new server, back). Not tried: F8 in the game, the HUD line and names over cars in the game, the key taps.
- Step 2 (commit "Phase 6 step 2"): the exe's LoadLibraryA import (0x783198; StartLANServer 0x5FA300 calls it with
  "server") goes through the plugin; each time server.dll loads, its imports go to vsock too (it imports Winsock
  **by ordinal**, WSAIoctl by name; htonl/htons/ntohl/ntohs #8/#9/#14/#15 stay Windows'). `out/win/srvprobe.exe`
  (`win/online/srvprobe.c`): the player's own server.dll (from `C:\Games\NFSU2-online`, never copied) loaded in a probe
  with the same patch, `StartServer("PROBEHOST", 0)`: 23 imports redirected, its WSAStartup brought the switch up,
  **it listens on its vip:9900, says `slave update (addr=10.77.14.2:9900)`, broadcasts on UDP 9999 (12 in 30 s), and
  a second process joined it over the switch** (`srvprobe join`, through 192.168.0.109: TCP connect accepted); clean
  StopServer and WSACleanup. Before the ordinal fix it ran on the real network (`addr=172.24.192.1`), which is what the
  game would have done. Not tried: hosting from the game's LAN menu (by hand) with the laptop joining.
- Step 3 (commit "Phase 6 step 3"): runtime/mprace.c builds into the plugin unchanged but for `#ifdef ROAM_HOST_WIN`
  at its includes, its `call` (roam_win_call) and the pause menu's hook (`pause_seen`, shared; natively UI_Pause's
  vtable 0x797CB8 entry 0 goes through `pause_msg_win`, a thiscall). `win/roam_asi/mprace_host_win.h` gives it the
  port's names (the log, getenv = the options, the clock, libm); `win/roam_asi/mprace_win.c` the rest: the FE screens
  (the FE manager's package lists), key taps (roam_keys.c), the car database lookup, the vtable patch, the career
  credit's core (event entry, prize, margin, RaceWon, the open races: the port's mpcredit.c without its sponsor
  offers and movie, which need the port's texts; the game offers sponsors at the next race), stand-ins for the
  browser's race statistics and quit datagrams (none: a quitter's fee stays, the others are not told). mprace_frame
  and mpcredit_frame run each frame; the player's keys are muted while the trip drives (`mprace_owns_input`). The
  plugin is 414,720 bytes: `asi_check.py`'s limit is now 512 KB (was 384). `out/win/mathtest.exe`: the plugin's
  logf/expf/powf/floorf against the C library's, 24,343 values, all exact (0 ulps). Nothing starts a trip yet (the
  lobby is step 4).
- Step 4 (commit "Phase 6 step 4"): `MPRACES` is 1 in the plugin too (the HELLO says RC_HF_LOBBY). Native pieces in
  roam_game.c (`#ifdef ROAM_HOST_WIN`): the race card's handler is `mp_card_msg` (shared; natively EngageRaceDialog's
  vtable 0x78F990 entry 0 goes through `mp_card_win`); no texts of our own in the game's language table, so the
  dialog's buttons are the game's own two labels of its yes/no questions (0xF770 left, 0x61C right, read from its 11
  calls of 0x558750) and the question names them by place ("LEFT button: ... RIGHT button: ..."); English texts only
  (`language_name` "english", `language_game_letters` ASCII); `mp_test_env` off. The two-player flow: the top line
  offers a lobby heard of elsewhere ("NAME hosts a multiplayer sprint (1 of 6), 1234 m away: press J to join from
  here"), `JoinKey = J` joins it from afar (`mp_far_join`) and leaves it again; `FarJoin = 1` (default) lets the host
  take a JOIN from afar from anyone in its room (not only friends) and keep that player wherever it drives
  (`far_mask`); the stage limit and "Friends & crew only" stay. roam_win.c: `roam_plat_lobby` 0 (no panel: the top
  line), notices on the top line, the lobby actions' queue. The plugin: 502,272 bytes, asi_check PASS; swaptest still
  17/17. **Not tried in the game: none of the lobby, the card's question, the trip.** The exe fingerprint does not
  cover the lobby's game functions (roam_fingerprint.py is still broken, see above).
- Step 5 (commit "Phase 6 step 5"): `Partner = <name>` in `mp_friend` (Windows only): that player is a friend in race
  lobbies, so `mp_trip_names` puts it in `MpRace.friends` and mprace.c's `credit_decide` gives `MPC_FRIEND` when it
  wins a race we finished that counts in our career (then the native mpcredit's `win_now` in the crib); it may also
  join from afar and past our stage. Logged once: `[roam] Partner "NAME": a friend in race lobbies ...`. Builds,
  asi_check PASS. The shared files (mprace.c, roam_game.c) also compile on the port's path (`-fsyntax-only` with a
  stand-in for the Mac's mach_time.h): no errors; the browser build itself was not run.

**Installed (2026-11-06 17:44):** the step-5 plugin (502,272 bytes, the same bytes as `out/win/NFSU2FreeRoam.asi`) in
`C:\Games\NFSU2-online\scripts`; the phase-4 plugin and .ini kept as `out/win/backup/NFSU2FreeRoam.phase4.*`. The test
copy's .ini got `Servers = local=ws://127.0.0.1:8691/lan, lan=ws://192.168.0.109:8691/lan`, `ServerKey = F8`,
`Labels = 1`, `JoinKey = J`, `FarJoin = 1`, `Partner = RishiLaptop` (Room stays empty: the laptop's .ini has none
either). **The laptop still has the phase-4 plugin:** copy both files in `C:\dev\laptop-update` (the plugin and a full
.ini: Name = RishiLaptop, Server = ws://192.168.0.109:8691/lan, Online = 1, Partner = Rishi, JoinKey = J) over its
`scripts` folder. Names compare case-insensitively (Partner, lobbies, the server's claims); they show as typed.

**To try in the game, in this order** (the player; the log lines to look for in `scripts\NFSU2FreeRoam.log`):
1. Start: `HUD hook: 0x006005e0 now jumps to the plugin`, `the exe's LoadLibraryA goes through the plugin`,
   `server switch: F8 goes to the next of the 2 server(s)`, then in free roam `[keys] the game's keyboard (...) hooked`.
   With the laptop in free roam: its name over its car (new natively), no FAULT line.
2. F8 in free roam: the top line "Server: lan (...): connecting", then "connected as 10.77.x.y"; the laptop's car goes
   and comes back. F8 in the Online menu: refused (the HUD is not up there, so the log says it, not the top line).
3. LAN menu by hand: Host LAN Server on the desktop (`[lan] server.dll (loaded at ...): the LAN server is on the
   virtual LAN`), the laptop's LAN menu finds it, joins, a race. (If this fails, the trips cannot work.)
4. A career race marker on the desktop: its card's Accept asks Solo or Multiplayer, with LEFT/RIGHT in its text (check
   which label words the two buttons show); Multiplayer: the top line "Multiplayer sprint: waiting for players".
   The laptop: drives there and presses Enter, or anywhere presses J on the offer ("Rishi hosts a multiplayer ...:
   press J to join from here"). The desktop: Enter starts it. Both games drive themselves to the LAN race (the
   player's keys are muted meanwhile: `[keys] the player's keys go to the race trip now`), race, results, back to the
   crib and free roam at the marker; `[mprace] ...` lines all the way, the prize in the bank.
5. Partner: both careers at the same stage, a race open in both; the laptop wins: the desktop's log
   `our friend won it: it counts` and `[mpcredit] ... won 1`; the race's marker gone from the desktop's map.

Known gaps (natively): no sponsor offers or race movie after a multiplayer win (the game offers sponsors after the
next race); no quit datagrams (a player who quits a race pays the fee, the others are not told: the game's own
network shows them leaving); the lobby's top line only shows while the HUD is up; English texts only; no controller
keys for the lobby; the trip has never run natively, so expect the first tries to stop somewhere: the `[mprace]`
lines say where.

**First test (2026-11-06 22:24–22:38, desktop + laptop on 192.168.0.109):** checklist steps 1, 2 and the lobby part
of 4 work: both in free roam, names over the cars, ghost cars; F8 switched local → lan and the laptop came back in
2.5 s; the race card's question (the left button is "Yes" = Multiplayer, the right "No" = Solo); the lobby opened,
the laptop joined from afar with J (Partner recognised), the host's Enter started the trip, both games drove out to
the LAN menus. **Then it failed:** "the LAN server did not start". server.dll had loaded and run (StartLANServer said
so) but on the *real* network: the log has no `[lan] server.dll ... is on the virtual LAN` line, every connect to the
host's 10.77.x.x:9900 was refused, and the manual Host LAN Server (step 3) failed the same way ("Connecting to
lobby...", then "lost your connection"). Cause, read from the running game: the exe's LoadLibraryA slot 0x783198 held
kernel32's own LoadLibraryA again (0x75c82310) — another module (the ASI loader or the d3d9.dll wrapper) put the
original back after the plugin hooked it; the ws2_32 slots stayed redirected. **Fix (step 6):** StartLANServer's own
`call [0x783198]` at 0x5FA311 (the exe's only load of "server") now reads a pointer cell of the plugin's
(`srv_callsite_patch`, log `StartLANServer's load of server.dll (0x005fa311) goes through the plugin`), and a frame
check logs `server.dll ... loaded without the plugin: it is on the REAL network` with the slot's owner if it ever
happens again. Built, asi_check PASS; not yet tried in the game (the running game held the file: install it after
quitting). Also seen: the laptop's game does not go full screen (its own display settings or d3d9 wrapper, not the
plugin).

**Second test (2026-11-06 23:08–23:42), the step-6 plugin on the desktop:** the LAN menu by hand works (server.dll
"is on the virtual LAN", listening on 10.77.25.2:9900, the laptop joined and raced). **The first native race trip
works end to end** (STAGE_1_SHORT_1, Street X, 4 laps): the desktop's card → Yes (Multiplayer), the laptop's J from
23 m, the host's Enter; both games drove out, the race ran, the results, the crib and back to free roam at the marker
in about 3.5 minutes. Desktop: 1st, "we won it: it counts", the career's $250 and the place's $150 top-up ($50 →
$450). Laptop: 2nd, Partner worked ("our friend won it: it counts", winner Rishi), $250 + $50 top-up; its way back
passed a "Connecting to lobby..." the trip called off by itself. No FAULT or FAILED on either side. Also fixed: the
vsock "unread for 20 s" warning counted from the last read, so the LAN server's 21 s keep-alives were reported as
stalls (it now counts from the data's arrival).

**The laptop hosting (2026-11-07 00:01–00:04):** a drift race (STAGE_1_DRIFT_1, mode 3) from the laptop's marker; the
desktop joined from afar with J at 1496 m, joined the laptop's LAN server at 10.77.29.2, raced, won (margin 100
points), the career took it ($450 → $850 with the top-up), back in free roam: a laptop host and a drift race both
work. Still untested: the laptop winning (Partner on the desktop's side), drag and the other race types, quitting a race.
The free-roam drop-outs seen at about 950 m (both sides' send rate falling, then "out of range" about every 65 s) are
the distance-based send rate and the hub's range, not a fault; a little hysteresis at the edge would stop the flapping.

**To do, the player's list (2026-11-07), in this order:**
1. **After a multiplayer win (IMPORTANT, the player's priority): built, not tried in the game yet.** The native
   mpcredit (win/roam_asi/mprace_win.c) now does runtime/mpcredit.c's steps 5 and 6: in the crib after the win, the
   sponsors the post-race list would offer (0x4A4610's test) asked one by one with the game's generic dialog and its
   own Yes / No labels ("Sponsor offer 1 of 3: <name>. Signing bonus X, its races pay Y. Sign with them?  Yes: sign.
   No: the next offer."; Yes signs, Career 0x532CF0), the answers to the crib's handler (UI_CareerCrib vtable 0x797550
   entry 0, 0x4AF3A0, through the plugin); then the race's movie (Scene07 + UpperClass into stage 2, the race table's
   scenes, Scene15 at the end) between Return to Explore Mode's save and the world. The trip waits meanwhile
   (mpcredit_busy) and the keys are the player's. Movies = 0 in the .ini skips the movie. Log: "[mpcredit] asked:",
   "signed with", "the race's movie".
2. Quitting a race mid-way: the other player is not told (no quit datagrams natively); they only see it in the
   game's own results.
3. The race lobby from a controller: only the keyboard now (J, Enter).
4. The lobby's line at the top: only shown while the HUD is up.
5. The car doors fix below: installed and tried in the game.
6. Partner and races not finished (below): the player's choice.

**To do, decided later (2026-11-07):**
- Partner and races not finished: the friend's win counts only in a race we finished. In the drift race the laptop was
  at 2 of 4 laps when the desktop finished, the game ended its race ("not finished"), so it got nothing. Choice for the
  player: count the friend's win whenever we stayed in to the end (only quitting loses it), or keep it as is.
- ToHost (commit 7b31bd8d, the joiner's car next to the host's after a race): **works** (00:30, a drag race
  STAGE_1_DRAG_1 the laptop hosted and won, $350 → $750 with the top-up; the desktop, back in free roam at 00:30:07,
  "jumps from 709 m away to its side" at 00:30:18). Drag races work too. The desktop's place was 0 ("not finished": the
  game ends the race when the winner is in), so Partner gave it nothing: the open choice above.
- The desktop's game hung once at 00:25:53, 40 s after its start, in the menus (Windows "stopped interacting", Id
  1002; the plugin's frames ran until 00:25:51, no fault): cause unknown, not seen again.

**The laptop's crash at the car dealer (2026-11-07 00:09), not the plugin:** SPEED2.EXE faulted at 0x513CCC (an
access violation in the game's own code): the dealer works out which cars are locked from GlobalB's car-unlock table
(0x838438, 12-byte entries: car type hash, two race hashes) and looks each car type up with 0x610130, which gave 0 for
one, read at +0x840. The laptop's game data is the desktop's (the same GlobalB.lzc and GLOBALA.BUN, add-on cars in it)
but it had no NFSU2Unlimiter (nor NFSU2_StanceMod), so the add-on cars were never made car types. Fixed by copying the
desktop's NFSU2Unlimiter.asi, its settings, UnlimiterData and NFSU2_StanceMod to the laptop (00:25, by USB, every
file's SHA-256 checked); the laptop also got the ToHost plugin build then. Not tried in the game yet. Both PCs need the
same mods: different car data breaks the dealer, and friends' cars would wear parts the other game lacks.

**Car doors, built (commit 097e4e6a), not tried in the game yet:** win/roam_asi/showoff_win.c, the native
runtime/showoff.c without its O key. The players open parts with the AnimationController mod (keys 1-5; it calls the
game's 0x61F820), which opens them on the car TYPE's node list. Now 0x620AE0 (a car's drawing) and 0x615A10 (its
lights) go through the plugin (a jump at each start, the 6 bytes replaced run from a page of the plugin's own): a
friend's car is drawn with its type's list holding that car's own nodes only; the player's open parts are read from
its type's list and sent (RC_OPEN_*); a friend's open parts open nodes of its own (0x433A80, 0x433D50). Only types the
game has animations in for (the player's own) open on friends' cars; others show closed. ShowOff = 0 in the .ini
turns the hooks off. Log lines "[showoff] ...".

**Seen in the second test (the doors issue, the fix above):** opening the car's doors (the game's door/hatch animation, standing still in free
roam) opens the doors of every player's car on that screen too, and the other player's screen shows none of it: the
door state is the game's own for all cars of that model (both are COROLLAs) and is not sent. Fix: keep the doors to
the player's own car, and send the door state with the car's state so the other screen shows it.

**Steps** (each built and checked before the next):
1. The DInput keyboard hook and the server switch (independent of the races, and useful to test with): F8 between two
   local entries (the same server under 127.0.0.1 and 192.168.0.109, so the swap is real), free roam's friend car gone
   and back, the vip new; refused in the Online lobby.
2. server.dll hooks; by hand: the desktop hosts from the LAN menu, the laptop finds and joins it, a race.
3. `mprace_host_win.h`, detours, libm; mprace.c + mpcredit.c built into the plugin (MPRACES on, UI off); `asi_check.py`
   PASS.
4. Lobby on: spots, the card, the top line, J. Desktop + laptop in one `Room`: drive-up join, the trip, the results,
   back at the spot, the prize in the bank (the test copy's profile); then the invite from afar.
5. `Partner =`: the laptop wins a race open in the desktop's career (both at the same stage) -> the desktop's career
   has it won (its event entry, the race's marker gone), the laptop's too; and without `Partner` the desktop's has not.

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
