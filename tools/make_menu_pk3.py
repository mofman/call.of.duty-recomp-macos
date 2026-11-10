"""Build gamedata/main/zz_cod1native.pk3: Options > Graphics menu with the widescreen r_mode entries.

python -I tools/make_menu_pk3.py [gamedata/main]

Reads ui/options_graphics.menu from localized_english_pak0.pk3, replaces the video mode cvarFloatList with the extended
list served by runtime/overrides/vidmodes.c, and writes a pk3 that sorts after the stock paks so it takes precedence.
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MENU = 'ui/options_graphics.menu'
OUT = 'zz_cod1native.pk3'

# (label, r_mode) in menu order; indices must match runtime/overrides/vidmodes.c
MODES = [
    ('@MENU_640480', 3), ('@MENU_800600', 4), ('@MENU_1024768', 6), ('@MENU_1152864', 7),
    ('@MENU_12801024', 8), ('@MENU_16001200', 9), ('@MENU_20481536', 10),
    ('1280x720 (16:9)', 13), ('1366x768 (16:9)', 14), ('1600x900 (16:9)', 15), ('1920x1080 (16:9)', 16),
    ('2560x1440 (16:9)', 17), ('3840x2160 (16:9)', 18),
    ('1280x800 (16:10)', 19), ('1440x900 (16:10)', 20), ('1680x1050 (16:10)', 21), ('1920x1200 (16:10)', 12),
    ('2560x1600 (16:10)', 22),
    ('Native display', 23),
]


def main():
    gamedir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'gamedata', 'main')
    with zipfile.ZipFile(os.path.join(gamedir, 'localized_english_pak0.pk3')) as z:
        text = z.read(MENU).decode('latin-1')
    lst = ' '.join(f'"{label}" {mode}' for label, mode in MODES)
    pat = re.compile(r'(cvar\s+"ui_r_mode"\s*\r?\n\s*cvarFloatList\s*\{)[^}]*(\})')
    text, n = pat.subn(lambda m: f'{m.group(1)} {lst} {m.group(2)}', text)
    if n != 1:
        sys.exit(f'expected one ui_r_mode cvarFloatList in {MENU}, found {n}')
    out = os.path.join(gamedir, OUT)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(MENU, text.encode('latin-1'))
    print(f'wrote {out}')


if __name__ == '__main__':
    main()
