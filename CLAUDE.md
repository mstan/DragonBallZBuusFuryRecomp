# CLAUDE.md - DragonBallZBuusFuryRecomp

This game repository defers platform behavior and debugging rules to:

- `gbarecomp/CLAUDE.md`
- `gbarecomp/PRINCIPLES.md`
- `gbarecomp/DEBUG.md`
- `gbarecomp/TCP.md` (this game uses port 19889)

## Build target

Initialize both submodules before building. Generated cart code is intentionally
not committed and must be recreated with `tools/regen.ps1` from a verified
user-supplied USA ROM. Build with MSYS2 mingw64, CMake, and Ninja. Keep large
generated translation units at modest parallelism and run compilation at low
priority.

## Game-specific rules

1. The supported ROM is USA revision 0 (`BG3E`), SHA-1
   `e65738e9d67688309f09811a54f495523ec9aada`. Do not weaken the identity gate.
2. ROMs, BIOS images, generated C/C++, saves, screenshots, and runtime caches
   are local artifacts and must never be committed.
3. Generated code is never edited. Fix `game.toml`, the recompiler, or the
   runtime and regenerate.
4. Adaptive widescreen is a game-owned optional mod. Native 240x160 rendering
   must remain unchanged when the feature is disabled.
5. Overworld extensions must resolve authored field-resource chunks. Do not
   repeat the hardware tilemap ring into expanded margins.
6. Visibility patches must target exact renderer reads. Do not mutate camera
   state or actor world coordinates to make expanded actors appear.
