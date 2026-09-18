# Banana Kong — Nintendo Switch port (Defold 1.13 / LuaJIT wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of
Banana Kong v2.0.0 on Switch homebrew. It contains no game code and no game
assets — it loads the game's own library and recreates, natively, the Android /
NativeActivity / JNI layer the Defold engine expects.
 
## Install & run
 
```
sdmc:/switch/bananakong
├── bananakong_nx.nro
├── libBananaKong.so     <- from your APK: lib/arm64-v8a/
├── assets/              <- the APK's assets/
├── config.txt           <- written on first launch
└── saves.txt            <- written on first launch
```

Launch via title override (hold R while starting an installed game).
 

## Controls
 
| Input | Action |
|---|---|
| A / X / D-pad up | Jump |
| B / L / ZL / D-pad down | Dive |
| Y / R / ZR / D-pad right | Dash |
| + | Back / pause |
| Left stick | Move the on-screen cursor |
| – | Toggle the cursor |
| Touchscreen | Tap (handheld) |
 
The cursor appears when you move the stick and hides after a couple of seconds
idle, so A goes back to jumping as soon as you stop steering; touching the
screen hides it too. While it's visible, A taps. The game also binds about
fifty debug and cheat keys — none of them are ever sent.
 
## Settings
 
`config.txt` is written next to the `.nro` on first launch:
 
```
language auto    # auto, en, de, es, fr, it, pt-BR, ru, tr
```
 
## Save editing
 
`saves.txt` is written on first launch with every field of the game's profile
commented out. Remove the `#` in front of a line to make changes that are
applied next boot:
 
```
bananaCurrent = 99999
goldenHeartCurrent = 25
hat5 = 2            # 0 = not owned, 1 = bought, 2 = equipped
tutorialEnabled = off
```
 
Play once before editing, so the profile exists. Values are overwritten where
they sit, leaving every other byte as the engine wrote it, and the original is
copied to `.bak` the first time.
 
## Building
 
Requires devkitPro with the switch-dev group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau
 
export DEVKITPRO=/opt/devkitpro
make                        # -> bananakong_nx.nro
make LOG=1                  # same, plus debug.log next to the .nro
```
 
Rebuilding libdrm_nouveau with `tools/libdrm_nouveau-astc-chipset-fix.patch` is
required; without it ASTC textures are software-decoded and the symptom is a
black screen with working audio. `PORTING_NOTES.md` has the engine details, the
save format and the bring-up log.
 
## Credits
 
The loader/shim infrastructure (so_util, the libc and pthread shims, fakefd,
opensles) derives from the open-source Switch .so-loader lineage — Andy Nguyen,
fgsfds and ChanseyIsTheBest, building on TheOfficialFloW's Vita/Switch loader
tradition — reaching this project via the Mulmash and Bloons Pop ports, with
the audout OpenSL ES backend from PvZ Fusion by way of Angry Birds Journey, the
ASTC chipset fix from Daggerfall Unity via Bloons Pop, and the save editor
design from Papa Pear Saga. The NativeActivity host, looper, JNI layer and
Defold/SDK behaviour are new for this port. All MIT-licensed. Thanks to
everyone in that lineage for making this approach possible.
