#!/bin/bash
# Rebuilds only the runtime inside an existing game bundle, reusing the cached recompiled-game object.
# usage: tools/fast_rebuild.sh <game-slug> "<App Name>"   (slug = build/recompiled_<slug>)
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
slug="$1"; app="$2"
d="$R/build/recompiled_$slug"
cache="$R/build/.fastcache"; mkdir -p "$cache"
CF="-DPS1_TRACE ${PS1_VERIFY:+-DPS1_VERIFY} ${DBG:+-g -fno-omit-frame-pointer} -O2 -arch arm64 -fobjc-arc -I$R/runtime/include -I$R/recompiler/include -I$d -Wno-everything"
# The cached object depends on runtime headers (struct layouts): rebuild it when any header is newer
obj="$cache/$slug${PS1_VERIFY:+.v}.o"
if [ ! -f "$obj" ] || [ -n "$(find "$R/runtime/include" "$d/recompiled_game.c" -newer "$obj" -print -quit)" ]; then
  clang $CF -c "$d/recompiled_game.c" -o "$obj"
fi
rt="$R/runtime/src"
clang $CF "$rt"/mimips1_{memory,cpu,interp,mdec,gte,gpu,spu,dma,timers,cdrom,bios,input,card,runtime}.c \
  "$R/recompiler/src/mips_disasm.c" "$rt"/platform/macos_{metal,audio,main}.m "$rt/recompiled_main.m" "$obj" \
  -framework Cocoa -framework Metal -framework MetalKit -framework AudioToolbox -framework GameController -framework CoreHaptics \
  -o "$R/build/$app.app/Contents/MacOS/$app"
echo "built $app"
