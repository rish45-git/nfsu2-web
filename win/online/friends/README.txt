NFSU2 ONLINE: FREE ROAM AND RACES WITH FRIENDS
===============================================

What this is
------------
The folder NFSU2-online is a complete, ready-to-play Need for Speed Underground 2 (PC, v1.2 North America) with
mods already in it, and a multiplayer plugin (scripts\NFSU2FreeRoam.asi): you drive around Bayview with your
friends, see their cars and names, and race them in the game's own races. Nothing to install: the game runs from
the folder. One of you hosts, the others join, and both take one step: "Play Online.bat".

What you need
-------------
- Windows 10 or 11, 64-bit.
- About 5 GB of disk space.
- Microsoft Visual C++ Redistributable 2015-2022, the x86 (32-bit) one, about 14 MB:
      https://aka.ms/vs/17/release/vc_redist.x86.exe
  One of the graphics mods (SpeedReflect) needs it. Install it even on 64-bit Windows: the game is a 32-bit
  program. If you already have it, the installer says so.
- A keyboard (the plugin's extra keys are keyboard keys; a controller works for driving).
Nothing else: no DirectX download, no .NET, no Linux, nothing from Steam or EA.

A note on antivirus: the mods are .asi files (plugins the game loads at start), and nfsu2-server.exe is a small
program nobody has seen before. Some antivirus programs flag such files. If the game starts without the mods or
files vanish, add the NFSU2-online folder to your antivirus's exclusions and copy it again.

Quick start
-----------
1. Copy the NFSU2-online folder anywhere you like (for example C:\Games\NFSU2-online), not inside Program Files.
2. Double-click "Play Online.bat" in that folder. Decide with your friends who hosts:

   ONE of you chooses   1  Host a game
      - A small window "NFSU2 online server" opens (minimised). That is the server: leave it open while you play.
      - The first time, Windows asks whether the firewall may let your friends in: click Yes.
      - The launcher shows the ADDRESSES FOR YOUR FRIENDS and puts the best one on your clipboard. Send it to them
        (paste it into your chat). It looks like 71.212.48.130.
      - The game starts by itself.

   EVERYONE ELSE chooses   2  Join a friend
      - Type or paste the address the host sent, press Enter. The game starts by itself.
      - Next time, 3 (Play) starts the game with the same address.

3. In the game, create a profile and start a career. The game plays its story intro and a first race (a few
   minutes); after that you are in free roam, and that is where the multiplayer lives. Nothing happens in the
   menus or in a Solo race.
4. In free roam your friends' cars appear with their names over them, and the top line of the HUD tells you what
   is going on.

Your name in the game is your profile's name. For another one, put it on the "Name =" line of
scripts\NFSU2FreeRoam.ini (letters and digits, up to 24 characters).

Who should host
---------------
The launcher tells the host which addresses work. Choose 5 (Check) to see it without changing anything.
- "over the internet: <an address>": friends anywhere can join with it. The launcher opened the port on your
  router by itself, or tells you the one thing to set in the router (forward TCP port 8691 to your PC).
- "over the internet: not possible from this PC": your internet provider shares one public address among many
  customers, so nobody can reach your PC from outside. Let a friend host instead. Or install Tailscale (free,
  https://tailscale.com) on both PCs and share your PC with your friends: the launcher then shows an
  "over Tailscale" address that works.
- "same Wi-Fi / LAN": for PCs in the same home.
Only one server is needed. If the host cannot play any more, anyone else can choose Host, and the rest Join the
new address.

Playing together
----------------
- Free roam: just drive. You see each other's cars, names and map arrows. F9 jumps your car next to a friend's.
  F7 jumps to your GPS destination (set one on the world map: M, pick a row and a place, "Activate GPS", then in
  the Yes/No box press Left and Enter; the default is No).
- Racing: at a race marker where the card asks "Solo or Multiplayer", choose Multiplayer: that opens a lobby.
  The others join with Enter at the same marker, or from anywhere by pressing J ONCE when the top line offers it
  (J again leaves). The one who opened the lobby presses Enter to start. All games drive themselves into the race
  and back to free roam afterwards. Do not quit a race before everyone has finished.
- Every race has two AI racers beside you (named Claude, ChatGPT, Gemini or DeepSeek). They get faster when a
  human leads one of them by 15 seconds. Solo career races are the game's own and unchanged.
- Away from the keyboard: after 30 s without input the game's AI drives your car (the HUD says so, your name
  shows [BOT] to others); any key gives it back. With a GPS destination set, the bot drives there.
- A friend's add-on car that your game does not have shows with stock parts. That is normal.

Keys the plugin adds
--------------------
J   join the race lobby the top line offers, from anywhere (again: leave it)
F7  fast travel to the GPS destination (or to the nearest race start when no GPS is set)
F9  fast travel next to a friend's car
All other keys are the game's own.

The launcher's choices
----------------------
1  Host a game    starts the server on this PC, opens the firewall and the router's port, starts the game
2  Join a friend  asks for the host's address, saves it, starts the game
3  Play           starts the game as it was set last time
4  Stop hosting   stops the server on this PC and closes the router's port again
5  Check          shows whether this PC can host and which addresses it has; changes nothing

What the server does and does not do
------------------------------------
- It only connects the games' plugins (TCP port 8691). It has no web page and hands out none of the game's files.
- Whoever has your address can join your games while the server window is open. Close it to stop.
- Its own files (who was on, logs) are in %LOCALAPPDATA%\NFSU2-online-server. Nothing is written into the game.
- When your router does not answer, the launcher asks api.ipify.org for your public address, to show it to you.
  Nothing else is sent anywhere.

If something goes wrong
-----------------------
- The game will not start, or starts and closes: the folder must not be read-only or under Program Files, and
  the screen must be awake (the plugin waits for it and says so in its log).
- You joined but see nobody: both of you must be in career free roam, not in a menu or a Solo race. The log
  scripts\NFSU2FreeRoam.log says "cannot reach the server" when the address is wrong or the host's server window
  is closed. The game keeps trying by itself and connects as soon as the server is there.
- The host's friends cannot connect: choose 5 (Check) on the host's PC and read the "over the internet" line.
- You see a friend's name but not the car: it is more than a few hundred metres away; press F9.
- A crash: before starting the game again, copy scripts\NFSU2FreeRoam.log somewhere. The next start overwrites
  it, and it is the only record of what happened.
