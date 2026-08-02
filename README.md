# DragonBallZBuusFuryRecomp

> This recompilation is a byproduct of developing
> [gbarecomp](https://github.com/mstan/gbarecomp). It is an in-development
> preservation and research project, not an official port.

Static recompilation of **Dragon Ball Z: Buu's Fury** for Game Boy Advance to
a native PC executable.

## Status

The USA release boots through its attract sequence and into gameplay using
statically generated native code. The project includes:

- the shared `recomp-ui` pre-boot launcher;
- strict ROM identity checking;
- a game-owned, optional adaptive-widescreen mod;
- authored streamed field chunks instead of repeated 256-pixel hardware
  tilemap rings;
- edge-anchored HUD placement;
- widened field-actor visibility without changing camera or world positions;
  and
- strict-static attract, gameplay, and traversal-fuzz validation.

The faithful 240x160 image is pixel-identical to the center of the enhanced
480x160 image. The enhanced view has been exercised in gameplay with zero
dispatch misses or interpreted instructions. This remains an early bring-up
and has not been exhaustively tested across the entire game.

## ROM

| Target | Region | Game code | SHA-1 | Debug port |
|---|---|---|---|---|
| `DragonBallZBuusFuryRecomp` | USA | `BG3E` | `e65738e9d67688309f09811a54f495523ec9aada` | 19889 |

The ROM is never distributed. Supply your own legally dumped copy at
`roms/dragon_ball_z_buus_fury_usa.gba`. The runtime refuses an unrecognized
image. Full CRC and SHA-256 identities are recorded in
[`baserom.md`](baserom.md).

## Quick start

1. Initialize the `gbarecomp` and `recomp-ui` submodules.
2. Supply the verified ROM above and a real GBA BIOS at
   `gbarecomp/bios/gba_bios.bin`.
3. Regenerate and build:

```powershell
git submodule update --init --recursive
.\tools\regen.ps1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target DragonBallZBuusFuryRecomp
```

Run `build/DragonBallZBuusFuryRecomp.exe`. The launcher remembers selected ROM
and presentation settings locally.

## Controls

| GBA button | Keyboard |
|---|---|
| D-Pad | Arrow keys |
| A | Z |
| B | X |
| Start | Enter |
| Select | Backspace |

Save states use **Shift+F1-F9** to save and **F1-F9** to load.

## Adaptive widescreen

Enable **Buu's Fury Adaptive Widescreen** in the launcher's Mods catalog. The
mod expands both the HUD and overworld as the normal window is resized, up to a
480x160 logical view. It leaves gameplay coordinates, actor world positions,
camera state, and the native 240x160 presentation unchanged.

## Legal

This repository contains no game ROM, GBA BIOS, generated code derived from
the ROM, or third-party decompilation source. You must supply legally obtained
copies. Dragon Ball and Dragon Ball Z are trademarks of their respective
owners. This is an unaffiliated, non-commercial preservation and research
effort.
