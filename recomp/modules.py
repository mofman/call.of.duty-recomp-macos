"""Guest modules lifted into the native build. Image bases do not overlap, so f_XXXXXXXX names are unique.

COD_TARGET selects the module set: 'sp' (default, CoDSP.exe -> generated/) or 'mp' (CoDMP.exe -> generated_mp/).
SP and MP DLLs share image bases, so each target gets its own analysis/generated dirs and its own binary.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.environ.get('COD_TARGET', 'sp')

if TARGET == 'sp':
    MODULES = [
        # name, file, entry override (real CRT entry; the shipped PE entry is a wrapper stub in .rsrc)
        dict(name='exe', file='gamedata/CoDSP.exe', entry=0x51dd4c,
             setjmp=[0x52e84c]),          # CRT _setjmp3: call sites get a host setjmp (see runtime/overrides/setjmp.c)
        dict(name='game', file='gamedata/gamex86.dll'),
        dict(name='cgame', file='gamedata/cgamex86.dll'),
        dict(name='ui', file='gamedata/uix86.dll'),
    ]
    ANALYSIS_DIR, GEN_DIR, OVERRIDES = 'analysis', 'generated', 'recomp/overrides.txt'
elif TARGET == 'mp':
    MODULES = [
        dict(name='exe', file='gamedata/CoDMP.exe', entry=0x560e1d,   # PE entry is the real CRT entry
             setjmp=[0x5729fc]),
        dict(name='game', file='gamedata/game_mp_x86.dll'),
        dict(name='cgame', file='gamedata/cgame_mp_x86.dll'),
        dict(name='ui', file='gamedata/ui_mp_x86.dll'),
    ]
    ANALYSIS_DIR, GEN_DIR, OVERRIDES = 'analysis/mp', 'generated_mp', 'recomp/overrides_mp.txt'
else:
    raise SystemExit(f'unknown COD_TARGET {TARGET!r} (sp|mp)')


def path(m, *rest):
    if m is None:
        return os.path.join(ROOT, *rest)
    return os.path.join(ROOT, *rest) if rest else os.path.join(ROOT, m['file'])

# Installed MP DLLs normally live in Main; accept root-level DLLs too.
for module in MODULES:
    candidate = os.path.join(ROOT, module['file'])
    if not os.path.isfile(candidate):
        data = os.path.join(ROOT, 'gamedata')
        if os.path.isdir(data):
            for directory in os.listdir(data):
                if directory.lower() == 'main':
                    nested = os.path.join(data, directory, os.path.basename(candidate))
                    if os.path.isfile(nested):
                        module['file'] = os.path.relpath(nested, ROOT)
                        break
