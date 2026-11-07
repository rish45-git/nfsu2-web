# Need for Speed: Underground 2 in the browser

A fan project that runs the PC game *Need for Speed: Underground 2* (2004) in Google Chrome and Firefox. The game's
x86 code is translated to C ahead of time and compiled to WebAssembly. Direct3D 9 runs on WebGPU and the sound on
Web Audio. You bring your own copy of the game.

Based on [NFSU2Recomp](https://github.com/TalhaMudo/NFSU2Recomp) by Talha Müderrisoğlu, the original x86 → C static
recompiler for NFSU2. This project was forked from
[its MIT-licensed version of 16 October 2026](https://github.com/TalhaMudo/NFSU2Recomp/tree/3d187ce7e5f1b99c0580866da4dbaa6d59dd23c7)
and uses only code from that version.

> Not affiliated with or endorsed by Electronic Arts. This repository contains none of the game's code or data.

![The start page: Play, the controls, your saves and Online](docs/web/start-page.png)

## What you need

- **The game:** your own PC copy, patched to v1.2 (North America), with this exact `SPEED2.EXE` (other versions
  will not work): 4,800,512 bytes, SHA-256 `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7`.
- **To build:** an Apple Silicon Mac (the tested setup) or Linux, Python 3, Node.js 20 or newer, Rust
  ([rustup](https://rustup.rs)), and [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases) 34 for that system
  unpacked into `deps/`. On Windows,
  build in WSL 2 with the clone in the Linux file system (`~/nfsu2-web`): under `/mnt/c` it builds much slower.
- **To play:** the latest Google Chrome, or Firefox where it has WebGPU (Windows, macOS).

## Build

```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
web/gen.sh "/path/to/Need for Speed Underground 2/SPEED2.EXE"   # translate the game's code to C (into gen/)
web/build.sh                                                    # compile it to out/web/nfsu2.wasm
```

The generated C and the compiled module are made from the game, so they are never committed (`.gitignore`).

## Play

```sh
web/start.sh --data "/path/to/Need for Speed Underground 2"
```

Open **http://localhost:8080/** in Chrome or Firefox and press **Play**. The start page has the settings, your saves
and the controls. The player's guide is [docs/web/PLAYING.md](docs/web/PLAYING.md). The game plays in any of its
thirteen languages, in Brazilian Portuguese, in Polish, in Turkish, in Hungarian, in Ukrainian, in Czech, in Slovak or in Greek; the
dubbed voices and movies and the Portuguese, Polish, Hungarian, Ukrainian, Czech and Slovak text come from mods (Czech's
with this repository's own texts for what it left in German or empty, `tools/lang/czech.json`), the Turkish text is this repository's own translation
(`tools/lang/turkish.json`, with a mod's fonts), the Greek text a contributor's (`tools/lang/greek.json`, with Greek
letters drawn into the game's fonts by `tools/greekfont`), as language packs. The menus and HUD can be redrawn at four times
their size, from a mod as a texture pack.

To play with friends, run the server behind HTTPS (other PCs need a secure context): `web/deploy/` has a Docker setup.
Everyone who plays from the same server can meet in free roam.

## How it works

- **`tools/`:** a Python lifter (Capstone) turns every function of `SPEED2.EXE` into C.
- **`runtime/`:** Windows, Direct3D 9, DirectSound and DirectInput, reimplemented. `runtime/web/` holds the browser
  parts.
- **`web/src/`:** the page. It runs the game's threads as workers on one shared memory, draws with WebGPU and plays
  sound through an AudioWorklet.
- **`web/server-rs/`:** the server, in Rust: serves the page, the module and your game folder, and connects players
  for multiplayer.

The same tree still builds the native macOS version
(`./gen.sh`, `./build.sh && ./link.sh`, `./run.sh`).

## The native PC plugin (branch `native-online`)

`NFSU2FreeRoam.asi` lets the original PC game (`SPEED2.EXE` v1.2 NA, unmodified, through the Ultimate ASI Loader)
play free-roam multiplayer with friends on this project's server, and race them in the game's own LAN races. It
changes the game only in memory. The detailed log, with every address and what was tried, is
[`PROGRESS.md`](PROGRESS.md). The player's guide that goes to the second PC is `laptop-update/README.md` (copied there
with the build; not in this repo).

**What it does (2026-11-07):**

- Free roam with friends: their cars, names, minimap arrows, doors and lights; F9 jumps next to a friend.
- Race lobbies from the career's race starts: Enter opens one, J joins from afar. A LAN race trip takes everyone
  there and back to free roam afterwards.
- AI racers in multiplayer races (`AiRacers = 2`, named Claude, ChatGPT, Gemini and DeepSeek): stock cars, rated a
  little above the best player's, top skill. They finish with their own times, and the results come in race order.
- No rubber band in multiplayer or career races (`CatchUp = 0`, `CareerCatchUp = 0`).
- F7 fast-travels to the GPS destination, or with no GPS set, to the nearest race start.
- An AFK bot drives your car after 30 s without input (`AfkBot = 30`), and a race bot keeps the route.
- The game keeps running with its window behind others or minimized (`RunInBackground = 1`), at 30 fps there
  (`BackgroundFps = 30`), so friends don't time out when you switch windows.
- Your Unlimiter add-on car (like the SENTRAB12) goes to friends with stock parts (`AddOnStockLook = 1`), since their
  games drew it with its own parts as only a name tag. `AddOnStockRace = 1` does the same in LAN races (built, not
  tried in a race yet).
- Guards for crashes of the game's own that the plugin's paths bring out (a sound bank that's gone).

**Build and check** (Windows, MSYS2's mingw32):

```sh
PATH="/c/msys64/mingw32/bin:$PATH" bash tools/build_roam_asi.sh       # -> out/win/NFSU2FreeRoam.asi
.venv/Scripts/python -I -X utf8 tools/asi_check.py out/win/NFSU2FreeRoam.asi "C:/Games/NFSU2-online/SPEED2.EXE"
PATH="/c/msys64/mingw32/bin:$PATH" bash win/online/build_probe.sh      # -> out/win/roambot.exe (test players)
```

Install: copy the `.asi` and `win/roam_asi/NFSU2FreeRoam.ini` into the game's `scripts\` folder. The server runs in
WSL (`win/online/wsl-server.sh`, port 8691). `roambot.exe ws://127.0.0.1:8691/lan synth:NAME:HASH n x y z seconds
ROOM radius` adds test players. They drive a fixed circle at one height and go through walls.

**Rules:**

- No EA game file is ever committed or uploaded.
- `SPEED2.EXE` is never modified.
- Test in `C:\Games\NFSU2-online`.
- Start the game once (`Start-Process`), never with computer use's `open_application`: it starts a new copy every
  time.

**Where to resume:** see "Where to resume" at the top of [`PROGRESS.md`](PROGRESS.md).

## Credits

Based on Talha Müderrisoğlu's [NFSU2Recomp](https://github.com/TalhaMudo/NFSU2Recomp) (its MIT version, see the
top), the static recompilation of NFSU2 for Apple Silicon, which in turn follows the method of
[N64: Recompiled](https://github.com/N64Recomp/N64Recomp) and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).
The language packs are built from KyoriAsh's [Multi 13 languages mod](https://nfsmods.xyz/mod/5749) (with Lee-Ryo's
Thai support, whose patch `runtime/language.c` follows), from TraduMix's and MixMods' Brazilian Portuguese
translation (Fyro, Beater, Kerlyston F.; Junior_Djjr, SideWinder, Elyzandro), from PLProjekt's Polish translation
(Daniel Graszka, MaDeR, Marcin Piech, Nati, Mateusz Wilkowski, alter), from VileStuff's
[English Subtitles Fixed](https://nfsmods.xyz/mod/5286), from muntazam's
[Turkish translation](https://www.moddb.com/mods/need-for-speed-underground-2-turkish-translation-patch) (its fonts), from
GameHunter's [Hungarian translation](http://www.gamehunter.hu/2010/08/need-for-speed-underground-2-pc.html)
(L_zooltan), from Clark X.'s [Ukrainian translation](https://kuli.com.ua/need-for-speed-underground-2), from ROXOR's and
PINĎA & syn's Czech translation and from MixerX's Slovak one ([abcgames.cz](https://www.abcgames.cz/?p=preklady_zobraz&id=7774)). The texture packs are built from LunicAura106's
[Upscaled HUD](https://nfsmods.xyz/mod/4319) and JeansBig's [Detailed Map](https://nfsmods.xyz/mod/4485), the
uncensored songs from xGMVx's [Uncensored & Restored Soundtrack](https://nfsmods.xyz/mod/3649), and Art Factory 28 to
34 from the underground2.net Team & Contributors' [PC port](https://nfsmods.xyz/mod/2004) of the Japanese release's
vinyls. Split screen is BirdSentinel (Zack)'s port of mihaits'
[NFSU2-SplitScreen](https://github.com/mihaits/NFSU2-SplitScreen). Other changes follow what these mods found out
about the game: ThirteenAG's, Aero_'s and xan1242's [Widescreen Fixes Pack](https://nfsmods.xyz/mod/3327), dstudnicki's
[NFSU2 High FPS Audio Fix](https://github.com/dstudnicki/NFSU2HighFPSAudioFix), the ExOpts Team's
[NFSU2 Extra Options](https://nfsmods.xyz/mod/290), MaxHwoy's [Show Hidden Races Mod](https://nfsmods.xyz/mod/1583)
and thelink2012's [Visual Reputation Remover](https://nfsmods.xyz/mod/2773). The start page's Mods tab lists them all.
Built with the help of Claude Code.

## License

MIT ([LICENSE](LICENSE)). *Need for Speed* and *Need for Speed: Underground 2* are trademarks of Electronic Arts Inc.
