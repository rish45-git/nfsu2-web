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
# roambot: simulated players on the real core (hosted C runtime, SSE2 floats like the plugin)
$CC -std=gnu11 -O2 -msse2 -mfpmath=sse -ffp-contract=off -Wall -Wno-unused-function -Iwin/online -Iruntime -c -o "$O/obj/rb_core.o" runtime/roam_core.c
$CC -std=gnu11 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Iwin/online -Iruntime -c -o "$O/obj/roambot.o" win/online/roambot.c
$CC -o "$O/roambot.exe" "$O/obj/roambot.o" "$O/obj/rb_core.o" "$O/obj/switch.o" -lwinhttp -lm
ls -la "$O/switch_probe.exe" "$O/roambot.exe"
