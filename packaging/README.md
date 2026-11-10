# macOS packages

Run `cmake --build build --target package_app` from the repository root.
The signed apps `Call of Duty Single Player.app` and
`Call of Duty Multiplayer.app` are written to `dist/`. SDL3, original binaries, game assets,
are embedded. Only the `Docs`, `Main`, `miles`, `pb`, and `Uninstall`
folders (case-insensitive) and `localization.txt` are copied from game data.
Other top-level mod folders are excluded; contents within `Main` are retained. Icon Composer compilation falls back to ICNS.

To seed personal saves, settings and registry data:
`./packaging/package_app.sh --with-saves build`

First launch copies game data to
`~/Library/Application Support/Call of Duty Recomp/game`.
Both apps share this directory; later launches and repackaging preserve it.
Move that directory aside to reset from a package's seed. Copying may require
additional disk space. Launch errors are appended to `launch.log` alongside it.
`COD_GAME_DIR` and `COD_BIN_DIR` still override the defaults.

These apps contain your installed game files and are for personal use.

Copy the complete patched retail installation into `gamedata/` before generating or packaging. Both targets read their executables and DLLs from there (including multiplayer DLLs in `Main/`); no binaries belong in `analysis/`.
