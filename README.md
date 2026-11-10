# Call of Duty Recomp for macOS

An experimental native Apple silicon recompilation of **Call of Duty** (the original 2003 game), with separate single-player and multiplayer executables. Python tools translate the Windows game's x86 code into C; a native runtime implements the Windows interfaces using SDL3, Metal, and macOS frameworks. The resulting arm64 executables run without Wine or Rosetta.

You must supply your own game files and compatible Windows binaries. This project is unofficial and is not affiliated with Infinity Ward, Activision, or the game's rights holders.

## Improvements

Implemented in this macOS version:

- Native Apple silicon execution, a Metal rendering backend, and SDL3 windowing and input.
- Separate Finder-launchable **Call of Duty Single Player.app** and **Call of Duty Multiplayer.app** bundles.
- Additional 16:9 and 16:10 resolutions, including 1920×1080, 2560×1440, 3840×2160, and 2560×1600, plus a native-display mode that accounts for the MacBook camera notch.
- Widescreen field-of-view correction that preserves the vertical view and expands the horizontal view, with corrected HUD proportions.
- Mouse-focus synchronisation when switching between the game and other apps.
- macOS fullscreen/windowed switching and Command-key shortcuts.
- Shared writable game data, saves, and settings outside the packaged apps.

This is an experimental project. Building both executables does not establish full campaign or multiplayer compatibility.

## Requirements

- An Apple silicon Mac. The current CMake configuration builds **arm64 only**.
- Xcode Command Line Tools for compilation and packaging. A full Xcode installation with Icon Composer support is needed to compile the modern `.icon` asset; packaging falls back to an ICNS icon when unavailable.
- Homebrew, CMake 3.24+, Ninja, SDL3, and Python 3 with Capstone.
- **A retail copy of the original Call of Duty, updated with the official 1.5 patch.**
- **A No-CD patch applied to the single-player executable (`CoDSP.exe`) after installing the 1.5 update.**

The runtime and overrides target specific executable layouts. Other versions, regional builds, or different No-CD executables may have different addresses and are not automatically compatible. No game files, official patches, or No-CD patches are provided by this project.

```sh
xcode-select --install
brew install cmake ninja sdl3 python
python3 -m venv .venv
source .venv/bin/activate
python -m pip install capstone
```

Run all subsequent commands from **the repository root**. Generating and compiling the translated game takes time and disk space; reduce the parallel job count if memory is limited.

## Obtain and prepare the game files

Install your retail copy on Windows, apply the **1.5 patch**, then apply the **single-player No-CD patch**. Copy the installed game files into `gamedata/`, preserving the directory structure and all game `.pk3` archives. Copy the installed files rather than disc installer archives.

Copy **everything from the installed game folder directly into `gamedata/`**. No separate binary-copy or archive-extraction step is needed for a complete patched retail installation:

```text
call.of.duty-recomp-macos/
  gamedata/
    CoDSP.exe                 # retail 1.5, with the single-player No-CD patch
    CoDMP.exe                 # retail 1.5 multiplayer executable
    gamex86.dll
    cgamex86.dll
    uix86.dll
    Main/
      game_mp_x86.dll
      cgame_mp_x86.dll
      ui_mp_x86.dll
      *.pk3                   # all installed game archives
    Docs/
    miles/
    pb/
    Uninstall/
    localization.txt
    ...                       # retain the remaining installed files
```

Both generation and runtime loading use this installed layout. Multiplayer DLLs can also be placed at the game-data root; root-level files take precedence. Use executables and DLLs from the same patched installation. `analysis/` and `analysis/mp/` contain generated analysis, not game binaries. Replacing an executable or DLL requires regenerating and rebuilding the matching target.

## Generate and build

Generate the analysis and C sources for both targets before configuring CMake:

```sh
source .venv/bin/activate
python recomp/analyze.py
python recomp/lift.py
COD_TARGET=mp python recomp/analyze.py
COD_TARGET=mp python recomp/lift.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 4
```

Single-player sources are written to `generated/`; multiplayer sources go to `generated_mp/`. CMake enables the multiplayer target when its generated dispatch table exists. Regenerate after changing the lifter or overrides, then rebuild. Runtime-only changes need a rebuild.

If SDL3 is not found, supply its CMake prefix:

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix sdl3)"
```

Launch from the repository root:

```sh
./build/cod1sp
./build/cod1mp
# Start single-player in a window at 1920×1080:
./build/cod1sp +set r_fullscreen 0 +set r_mode 16
```

`COD_GAME_DIR` overrides the game-data directory. `COD_BIN_DIR` overrides the Windows binary directory, which defaults to the game-data directory for both targets. DLLs are also looked up in its `Main/` subdirectory. Packaged apps use their embedded binaries by default. Use absolute paths when overriding these locations.

### Widescreen and controls

The runtime adds widescreen modes to the original resolution table. Use `+set r_mode 16` for 1920×1080, `+set r_mode 17` for 2560×1440, or `+set r_mode 23` for the native display resolution.

To add the extended resolution list to the graphics menu for an English installation:

```sh
python -I tools/make_menu_pk3.py gamedata/Main
```

This creates `zz_cod1native.pk3` using `localized_english_pak0.pk3` from your installation. Adjust the directory's spelling to match your files. The tool expects the English menu archive; other languages may need adaptation.

| Action | Control |
|---|---|
| Switch to another app | **Command+Tab** |
| Toggle fullscreen/windowed | **Command+Return** or **Option+Return** |
| Quit | **Command+Q**; save your progress first |

Widescreen field-of-view and HUD corrections are enabled by default. Set `COD_VERTMINUS=1` to restore the original field-of-view behaviour or `COD_HUD_STRETCH=1` to disable HUD aspect correction.

## Configuration and saves

Unbundled runs use the files in `gamedata/`. Packaged apps share a writable copy at:

```text
~/Library/Application Support/Call of Duty Recomp/
  game/                       # installed data, configurations, saves, registry data
  launch.log                  # launch and runtime diagnostics
```

The first packaged launch copies the bundled game data into this directory, which requires additional disk space. Both apps use the same copy. Later launches and repackaging preserve existing user files; changes to a bundle's seed data do not replace them. Back up this directory before experimenting with saves or settings. Move `game/` aside to seed a fresh copy on the next launch.

For multiplayer, `COD_CDKEY` can seed the emulated registry with your own retail CD key. Treat that key as private and omit it from shared logs or reports.

## Package macOS apps

With both targets built and the game files prepared:

```sh
cmake --build build --target package_app
open "dist/Call of Duty Single Player.app"
open "dist/Call of Duty Multiplayer.app"
```

Or run the script directly:

```sh
./packaging/package_app.sh build
# Optionally include personal saves, settings, and registry data as seed files:
./packaging/package_app.sh --with-saves build
```

Packaging embeds SDL3, the Windows binaries, and game data; creates the app icons and metadata; and ad-hoc signs and verifies both apps. The signature is not Developer ID signing or notarisation.

Only the `Docs`, `Main`, `miles`, `pb`, and `Uninstall` folders (case-insensitive) and `localization.txt` are copied from the game installation by default. Other top-level mod folders are excluded; contents within `Main` are retained. Personal saves, `.cfg` files, and registry data are omitted unless `--with-saves` is used.

**These apps contain your installed game files. Keep them for personal use; do not upload them as public releases.**

## Checks and troubleshooting

For a manual smoke test, launch single-player, load a mission, check audio and mouse input, change resolution, switch apps, save, quit, and reload. Check multiplayer separately with your intended server. Full campaign completion, server compatibility, regional editions, and older macOS versions are not certified here.

- **Missing generated sources:** run analysis and lifting for both targets before configuring CMake.
- **Missing game data:** check that the installed archives are directly inside `gamedata/Main/`, rather than nested inside another installation directory. Launch unbundled executables from the repository root.
- **Crashes or missing dispatch targets:** confirm the retail 1.5 installation, the single-player No-CD executable, and matching DLLs. Regenerate and rebuild after changing any input binary. Include the terminal log and reproduction steps in a report.
- **Extended resolutions absent from the menu:** generate the menu archive above, or select a mode through command-line arguments.
- **Packaged app still uses old settings or data:** both apps retain their existing Application Support copy. Back it up and move it aside if you want to reseed from a rebuilt app.
- **Finder launch fails:** inspect `~/Library/Application Support/Call of Duty Recomp/launch.log`. Initial seeding errors may require launching the executable inside the app's `Contents/MacOS/` directory from a terminal to see diagnostics.

## Credits

- Original game: Infinity Ward / Activision and the respective rights holders.
- SDL3 for platform services, Capstone for x86 disassembly, and Apple's Metal and macOS frameworks for native rendering and integration. External dependencies retain their own licences.
