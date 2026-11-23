#!/bin/bash
# The online add-on for a game folder: the launcher and the server for Windows, as a folder and a zip.
#
#   bash win/online/friends/build.sh            (Git Bash on Windows, with Rust for x86_64-pc-windows-msvc)
#
#   out/win/friends/Play Online.bat             the launcher: Host a game / Join a friend / Play
#   out/win/friends/online/Play-Online.ps1
#   out/win/friends/online/nfsu2-server.exe     web/server-rs built for Windows, the C runtime linked in (no
#                                               redistributable needed); run with --friends by the launcher
#   out/win/friends/HOW TO PLAY ONLINE.txt      win/online/friends/README.txt, under the name a player looks for
#   out/win/NFSU2-online-addon.zip              the same, to unpack into a game folder that has the plugin
#
# Nothing of the game is in it.  The plugin (NFSU2FreeRoam.asi and its .ini) is built and shipped as before.
set -e -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="$REPO/out/win/friends"
cd "$REPO/web/server-rs"
RUSTFLAGS='-C target-feature=+crt-static' cargo build --release -p nfsu2-server
rm -rf "$OUT"; mkdir -p "$OUT/online"
cp target/release/nfsu2-server.exe "$OUT/online/"
cp "$HERE/Play-Online.ps1" "$OUT/online/"
# (cmd and Notepad want CRLF)
sed 's/\r$//; s/$/\r/' "$HERE/Play Online.bat" > "$OUT/Play Online.bat"
sed 's/\r$//; s/$/\r/' "$HERE/README.txt" > "$OUT/HOW TO PLAY ONLINE.txt"
rm -f "$REPO/out/win/NFSU2-online-addon.zip"
(cd "$OUT" && powershell.exe -NoProfile -Command "Compress-Archive -Path * -DestinationPath '..\\NFSU2-online-addon.zip' -Force")
ls -la "$OUT" "$OUT/online" "$REPO/out/win/NFSU2-online-addon.zip"
sha256sum "$OUT/online/nfsu2-server.exe" "$REPO/out/win/NFSU2-online-addon.zip"
