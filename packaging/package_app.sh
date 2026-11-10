#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build}
WITH_SAVES=0
if [ "$BUILD" = --with-saves ]; then WITH_SAVES=1; BUILD=${2:-build}; fi
for mode in sp mp; do
 BIN="$BUILD/cod1$mode"
 [ -x "$BIN" ] || { echo "Missing $BIN"; exit 1; }
 NAME='Call of Duty Single Player'
 if [ "$mode" = mp ]; then NAME='Call of Duty Multiplayer'; fi
 APP="dist/$NAME.app"; C="$APP/Contents"
 mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/bin" "$C/Resources/game"
 cp "$BIN" "$C/MacOS/cod1$mode"
 COD_TARGET="$mode" python3 - "$C/Resources/bin" <<'PYBIN'
import shutil, sys
from pathlib import Path
sys.path.insert(0, 'recomp')
from modules import MODULES, ROOT
for module in MODULES:
    shutil.copy2(Path(ROOT) / module['file'], sys.argv[1])
PYBIN
 python3 - "$C/Resources/game" "$WITH_SAVES" <<'PY'
import sys, shutil
from pathlib import Path
out=Path(sys.argv[1])
shutil.rmtree(out); out.mkdir()
def ignore(folder, names):
    return [n for n in names if n == '.DS_Store' or n == 'registry-candidates.txt' or n.endswith(('.log','.dat')) or (sys.argv[2]=='0' and (n.lower() in ('save','saves','registry.txt') or n.endswith('.cfg')))]
allowed = {'docs', 'main', 'miles', 'pb', 'uninstall'}
for source in Path('gamedata').iterdir():
    if source.is_dir() and source.name.lower() in allowed:
        shutil.copytree(source, out/source.name, ignore=ignore)
    elif source.is_file() and (source.name.lower() == 'localization.txt' or
                              (sys.argv[2] == '1' and source.name.lower() == 'registry.txt')):
        shutil.copy2(source, out/source.name)
PY
 SDL_REF=$(otool -L "$BIN" | awk '/libSDL3/ {print $1; exit}')
 [ -n "$SDL_REF" ] || { echo 'SDL3 dependency missing'; exit 1; }
 SDL_NAME=$(basename "$SDL_REF")
 cp -L "$SDL_REF" "$C/Frameworks/$SDL_NAME"
 chmod u+w "$C/Frameworks/$SDL_NAME"
 install_name_tool -id "@rpath/$SDL_NAME" "$C/Frameworks/$SDL_NAME"
 install_name_tool -change "$SDL_REF" "@executable_path/../Frameworks/$SDL_NAME" "$C/MacOS/cod1$mode"
 ICON=assets/AppIcon.icon
 source packaging/icon.sh
 python3 - "$C/Info.plist" "$NAME" "$mode" <<'PY'
import plistlib,sys
with open(sys.argv[1],'wb') as f:
 plistlib.dump(dict(CFBundleName=sys.argv[2],CFBundleDisplayName=sys.argv[2],CFBundleIdentifier='local.cod1native.'+sys.argv[3],CFBundleExecutable='cod1'+sys.argv[3],CFBundlePackageType='APPL',CFBundleShortVersionString='1.0',CFBundleVersion='1',CFBundleIconFile='AppIcon',CFBundleIconName='AppIcon',NSHighResolutionCapable=True,LSApplicationCategoryType='public.app-category.action-games'),f)
PY
 codesign --force --sign - "$C/Frameworks/$SDL_NAME"
 codesign --force --sign - "$APP"
 codesign --verify --strict "$APP"
done
