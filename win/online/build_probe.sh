#!/bin/bash
# Builds out/win/switch_probe.exe (32-bit, like the game).  switch.c is compiled with the plugin's flags
# (freestanding, no C runtime) to prove it can go into the ASI; only probe.c uses the C runtime.
#   PATH="/c/msys64/mingw32/bin:$PATH" bash win/online/build_probe.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
CC="${MINGW_CC:-i686-w64-mingw32-gcc}"; O=out/win; mkdir -p "$O/obj"
$CC -std=c11 -O2 -march=i686 -msse2 -mfpmath=sse -mstackrealign -ffreestanding -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Wall -Wextra -Iwin/online -c -o "$O/obj/switch.o" win/online/switch.c
$CC -std=c11 -O2 -Wall -Wextra -Iwin/online -c -o "$O/obj/probe.o" win/online/probe.c
$CC -o "$O/switch_probe.exe" "$O/obj/probe.o" "$O/obj/switch.o" -lwinhttp
# vsprobe: the virtual Winsock (vsock.c, vsock_ws2.c with the plugin's flags and its libc) against a running server
FREE="-std=c11 -O2 -march=i686 -msse2 -mfpmath=sse -mstackrealign -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables  -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0602 -Iwin/online"
$CC $FREE -c -o "$O/obj/vsock.o" win/online/vsock.c
$CC $FREE -c -o "$O/obj/vsock_ws2.o" win/online/vsock_ws2.c
$CC $FREE -fno-builtin -fno-tree-loop-distribute-patterns -Iwin/roam_asi -Iruntime -c -o "$O/obj/vp_libc.o" win/roam_asi/roam_libc.c
$CC -std=c11 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0602 -Iwin/online -c -o "$O/obj/vsprobe.o" win/online/vsprobe.c
$CC -o "$O/vsprobe.exe" "$O/obj/vsprobe.o" "$O/obj/vsock_ws2.o" "$O/obj/vsock.o" "$O/obj/switch.o" "$O/obj/vp_libc.o" -lwinhttp -lws2_32   # ws2_32: htons and friends only
# roambot: simulated players on the real core (hosted C runtime, SSE2 floats like the plugin)
$CC -std=gnu11 -O2 -msse2 -mfpmath=sse -ffp-contract=off -Wall -Wno-unused-function -Iwin/online -Iruntime -c -o "$O/obj/rb_core.o" runtime/roam_core.c
$CC -std=gnu11 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Iwin/online -Iruntime -c -o "$O/obj/roambot.o" win/online/roambot.c
$CC -o "$O/roambot.exe" "$O/obj/roambot.o" "$O/obj/rb_core.o" "$O/obj/switch.o" -lwinhttp -lm
ls -la "$O/switch_probe.exe" "$O/vsprobe.exe" "$O/roambot.exe"
# swaptest: the plugin's server switch (win/roam_asi/roam_win.c) on the plugin's own switch, core and vsock objects
$CC -std=gnu11 -O2 -msse2 -mfpmath=sse -mstackrealign -ffp-contract=off -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0602 \
    -Iwin/online -Iwin/roam_asi -Iruntime -c -o "$O/obj/st_roamwin.o" win/roam_asi/roam_win.c
$CC -std=gnu11 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Iwin/online -Iruntime -c -o "$O/obj/swaptest.o" win/online/swaptest.c
$CC -o "$O/swaptest.exe" "$O/obj/swaptest.o" "$O/obj/st_roamwin.o" "$O/obj/rb_core.o" "$O/obj/switch.o" "$O/obj/vsock.o" "$O/obj/vsock_ws2.o" -lwinhttp -lws2_32 -lm
ls -la "$O/swaptest.exe"
# srvprobe: the game's server.dll (from the player's game folder) on the virtual LAN, a host and a joiner
$CC -std=c11 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0602 -Iwin/online -c -o "$O/obj/srvprobe.o" win/online/srvprobe.c
$CC -o "$O/srvprobe.exe" "$O/obj/srvprobe.o" "$O/obj/vsock_ws2.o" "$O/obj/vsock.o" "$O/obj/switch.o" "$O/obj/vp_libc.o" -lwinhttp -lws2_32
ls -la "$O/srvprobe.exe"
# qtprobe: a race's quit notice (N2QT on UDP 3659) between two processes on the switch, as mprace_win.c sends it
$CC -std=c11 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0602 -Iwin/online -c -o "$O/obj/qtprobe.o" win/online/qtprobe.c
$CC -o "$O/qtprobe.exe" "$O/obj/qtprobe.o" "$O/obj/vsock_ws2.o" "$O/obj/vsock.o" "$O/obj/switch.o" "$O/obj/vp_libc.o" -lwinhttp -lws2_32
ls -la "$O/qtprobe.exe"
# mathtest: the plugin's logf/expf/powf/floorf (mprace_win.c) against the C library's (needs tools/build_roam_asi.sh first: its objects)
$CC -std=gnu11 -O2 -msse2 -mfpmath=sse -Wall -o "$O/mathtest.exe" win/online/mathtest.c "$O/obj/mprace_win.o" "$O/obj/roam_libc.o" -lm
