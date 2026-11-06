#!/bin/bash
# A local nfsu2 server for testing the native Windows client, in WSL 2.
#
#   bash wsl-server.sh setup    once: the C compiler and Rust (asks for your sudo password)
#   bash wsl-server.sh build    build nfsu2-server and nfsu2-roamload from this checkout
#   bash wsl-server.sh run      start the server: http://127.0.0.1:8080, /lan for the clients (Ctrl+C stops it)
#   bash wsl-server.sh bots [n] [seconds]   (a LOAD generator: its cars are not valid for a real game; for a game use
#                               out/win/roambot.exe, win/online/roambot.c, built by build_probe.sh)
#                               n simulated free-roam players (default 3, 600 s) on the running server, driving
#                               round the career start (NFSU2_BOT_CENTER="x,y,z" moves them; free roam only links
#                               players near each other, and only in the same room: the bots are in room 0, as a
#                               player with no Room in the .ini; NFSU2_BOT_ROOM=<hash> for another)
#
# Only the /lan WebSocket is needed by the native client, so neither the browser page (web/dist) nor the game
# module (out/web/nfsu2.wasm) has to be built.  The server reads the game folder (--data: it insists on finding
# SPEED2.EXE there) and never writes into it.  Its build and its state stay in WSL's own file system (faster than
# /mnt/c, and nothing of it lands in the repository).
set -e -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
GAME="${NFSU2_DATA:-/mnt/c/Games/NFSU2-online}"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$HOME/.cache/nfsu2-native/target}"
STATE="${NFSU2_STATE:-$HOME/.local/state/nfsu2-native}"
PORT="${NFSU2_HTTP_PORT:-8080}"
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
BIN="$CARGO_TARGET_DIR/release"

case "${1:-}" in
  setup)
    sudo apt-get update
    sudo apt-get install -y build-essential pkg-config curl git
    if ! command -v cargo >/dev/null; then
      curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
      . "$HOME/.cargo/env"
    fi
    rustc -V; cargo -V; cc --version | head -1
    ;;
  build)
    cd "$REPO/web/server-rs"
    cargo build --release -p nfsu2-server -p nfsu2-roamload
    ls -la "$BIN/nfsu2-server" "$BIN/nfsu2-roamload"
    ;;
  run)
    [ -f "$GAME/SPEED2.EXE" ] || { echo "no SPEED2.EXE in $GAME (NFSU2_DATA=<folder> to use another)"; exit 1; }
    mkdir -p "$STATE"
    echo "server: http://127.0.0.1:$PORT  (/lan for the clients), state in $STATE, game folder $GAME (read only)"
    exec "$BIN/nfsu2-server" --data "$GAME" --state "$STATE" --http-port "$PORT"
    ;;
  bots)
    export NFSU2_BOT_CENTER="${NFSU2_BOT_CENTER:-1695,-883,24}"
    exec "$BIN/nfsu2-roamload" --base "http://127.0.0.1:$PORT" --n "${2:-3}" --seconds "${3:-600}" --spread 150 --room "${NFSU2_BOT_ROOM:-0}"
    ;;
  *)
    sed -n 2,9p "$0"; exit 1
    ;;
esac
