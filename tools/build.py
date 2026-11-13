#!/usr/bin/env python3
"""Build the port from your own copy of the game.

    python tools/build.py [tfx|config|all] [--game DIR] [--regen] [--force] [--jobs N]

  tfx     build/tfx/tfx.exe         from TFX.DAT    (the game)
  config  build/config/tfxconfig.exe from CONFIG.EXE (its setup program)
  all     both (the default)

The game's code is not in this repository: the recompiler (tools/recomp.py)
translates the TFX.DAT / CONFIG.EXE in your game folder into C, which is then
compiled together with the runtime (runtime/).  The game folder is remembered
in build/game_dir.txt after the first --game.

Needs, in toolchain/ (build.bat / tools/setup.ps1 fetch them):
  llvm-mingw-*-x86_64     the compiler
  SDL2-*                  SDL2's MinGW development package
and Python 3.9+ with the capstone package to run this.  Optional:
  fidelityfx-*            AMD FidelityFX SDK   -> upscaler=fsr
  streamline-*            NVIDIA Streamline SDK -> upscaler=dlss, frame generation
Without them the port builds with its own upscaler only.

Environment: XCFLAGS (extra compiler flags), CONSOLE=1 (console build),
INTERP=addr,... (always interpret these functions), EXTRA (extra recomp.py
arguments), TFX_TOOLCHAIN (another toolchain folder).
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.environ.get('TARGET', 'x86_64')

# The hooks, patches and observed self-modification lists in this repository
# fit exactly these files (SHA-256).  TFX.DAT: 1,032,503 bytes; CONFIG.EXE:
# 282,334 bytes (the CD-ROM release, English, as installed by its INSTALL.EXE).
SUPPORTED = {
    'TFX.DAT': '2cc41a39fa95a2c9fd883c4220d7bdd9cca00c543c89b019eb270763f59d1d9a',
    'CONFIG.EXE': 'eaccf8f6d05f7a5dbf27e836cc10c6561c381334aa1339615e09b6c7fbff90be',
}
PROGRAMS = {
    'tfx': dict(src='TFX.DAT', exe='tfx',
                defs=['-DPROGRAM_FILE="TFX.DAT"', '-DDEFAULT_ENV="DID=D:"', '-DDEFAULT_ARGS="tfx"', '-DREQUIRE_CD=1',
                      '-DMUSIC_DRIVER=1', '-DTFX_GAME=1']),
    'config': dict(src='CONFIG.EXE', exe='tfxconfig', defs=['-DPROGRAM_FILE="CONFIG.EXE"']),
}
# Other versions recognised, to say what the user has and what to do.
# Keep in step with $Known in tools/setup.ps1.
PATCH_ADVICE = ('Install TFX from its CD and apply the official CD patches tfxcd_1, tfxcd_2 and tfxcd_3, '
                'in that order (they take the game to version 2.01).')
KNOWN = {
    'fe2c8d787454ebfac1e2895be1e82fa89c846526894e4d611d8038e847e2d6fb':
        ('the TFX.DAT on the CD itself: the original, unpatched CD release (1,013,255 bytes)', PATCH_ADVICE),
}


def identify(path, which):
    """(ok, text): is this the supported TFX.DAT / CONFIG.EXE?  The text says
    what it is when it isn't, and what to do."""
    data = open(path, 'rb').read()
    sha = hashlib.sha256(data).hexdigest()
    if sha == SUPPORTED[which]:
        return True, f'{which}: the supported version ({len(data):,} bytes)'
    what, advice = KNOWN.get(sha, (None, None))
    if not what:
        size = {'TFX.DAT': 1032503, 'CONFIG.EXE': 282334}[which]
        if data[:2] != b'MZ':
            what, advice = 'not a DOS program at all', f'Choose the folder of the installed game, where {which} is the game\'s program file.'
        elif len(data) == size:
            what, advice = ('the right size but different contents: changed by a crack, a trainer or another patch, or damaged',
                            'Reinstall the game from its CD and apply the official CD patches 1 to 3.')
        else:
            what, advice = (f'an unknown version ({len(data):,} bytes; the supported one has {size:,})',
                            'The port needs the CD-ROM release patched to version 2.01. ' + PATCH_ADVICE)
    return False, (f'{which} in this folder is {what}.\n{advice}\n'
                   f'  yours:     SHA-256 {sha}\n  supported: SHA-256 {SUPPORTED[which]}\n'
                   'The port works on the game\'s code at fixed addresses, so it needs exactly that file.')


LIBS = ('-lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi '
        '-lshell32 -ldinput8 -ld3d12 -ldxgi -ld3dcompiler -ldxguid -lwintrust -lcrypt32').split()


def die(msg):
    print('\nBUILD FAILED: ' + msg)
    sys.exit(1)


def find_one(pattern, what, required=True):
    hits = sorted(glob.glob(pattern))
    if not hits:
        if required:
            die(f'{what} not found ({pattern}).\nRun build.bat (or tools\\setup.ps1) to fetch the build tools.')
        return None
    return hits[-1]


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def game_dir(arg):
    """--game, else the folder remembered from last time, else this checkout's own layout."""
    memo = os.path.join(ROOT, 'build', 'game_dir.txt')
    cands = [arg] if arg else []
    if os.path.exists(memo):
        cands.append(open(memo, encoding='utf-8').read().strip())
    cands.append(os.path.join(ROOT, '_extract', 'installed', 'TFX', 'TFX'))
    for d in cands:
        if not d:
            continue
        d = os.path.abspath(d)
        for sub in ('', 'TFX', os.path.join('TFX', 'TFX')):          # the folder itself or the one above it
            g = os.path.join(d, sub)
            if os.path.isfile(os.path.join(g, 'TFX.DAT')):
                if arg:
                    os.makedirs(os.path.dirname(memo), exist_ok=True)
                    open(memo, 'w', encoding='utf-8').write(g)
                return g
        if arg:
            die(f'no TFX.DAT in "{d}".\nGive the folder of the installed game: the one that holds TFX.DAT and CONFIG.EXE.')
    die('the game folder is not known yet.\nRun: python tools/build.py --game "<folder with TFX.DAT>"   (or build.bat)')


def run(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', **kw)
    return r.returncode, r.stdout


def newer(a, b):
    return os.path.getmtime(a) > os.path.getmtime(b)


def build(name, game, a, tc):
    prog = PROGRAMS[name]
    src = os.path.join(game, prog['src'])
    if not os.path.isfile(src):
        die(f'{prog["src"]} is not in the game folder {game}')
    data = open(src, 'rb').read()
    sha = hashlib.sha256(data).hexdigest()
    ok, text = identify(src, prog['src'])
    if not ok and not a.force:
        die(text + '\n(--force builds anyway; expect it to fail at run time.)')
    gen = os.path.join(ROOT, 'build', name + '_gen')
    out = os.path.join(ROOT, 'build', name)
    obj = os.path.join(out, 'obj')
    os.makedirs(obj, exist_ok=True)
    xcflags = os.environ.get('XCFLAGS', '').split()

    # compiler flags changed, or the optional SDKs came or went: rebuild everything
    stamp = ' '.join([TARGET] + xcflags + ['ffx' if tc['ffx'] else 'noffx', 'sl' if tc['sl'] else 'nosl'])
    stamp_file = os.path.join(obj, '.xcflags')
    if stamp_file_text(stamp_file) != stamp:
        for f in glob.glob(os.path.join(obj, '*.o')):
            os.remove(f)
        open(stamp_file, 'w').write(stamp)

    # 1. the game's code -> C (again when the game file is another one)
    src_stamp = os.path.join(gen, '.source')
    if a.regen or not os.path.exists(os.path.join(gen, 'functab.c')) or stamp_file_text(src_stamp) not in ('', sha):
        print(f'[{name}] translating {prog["src"]} to C (the first time includes a code analysis of a few minutes) ...', flush=True)
        t0 = time.time()
        cmd = [sys.executable, 'recomp.py', src, gen]
        if os.environ.get('INTERP'):
            cmd += ['--interp', os.environ['INTERP']]
        cmd += os.environ.get('EXTRA', '').split()
        r = subprocess.run(cmd, cwd=os.path.join(ROOT, 'tools'))
        if r.returncode:
            die('the recompiler failed (see above)')
        print(f'[{name}] translated in {time.time() - t0:.0f} s', flush=True)
    open(src_stamp, 'w').write(sha)

    # 2. compile
    cflags = ['-O1', '-g', '-fno-strict-aliasing', '-fwrapv', '-w', '-I' + os.path.join(ROOT, 'runtime'), '-I' + gen,
              '-I' + os.path.join(tc['sdl'], 'include', 'SDL2'), '-DSDL_MAIN_HANDLED',
              f'-DPROGRAM_HASH=0x{fnv1a64(data):016x}ull'] + prog['defs'] + xcflags
    if tc['ffx']:
        cflags += ['-I' + os.path.join(tc['ffx'], 'api', 'include'), '-I' + os.path.join(tc['ffx'], 'upscalers', 'include')]
    else:
        cflags.append('-DNO_FSR')
    jobs = []
    cpu_h = os.path.join(ROOT, 'runtime', 'cpu.h')
    for f in sorted(glob.glob(os.path.join(gen, '*.c'))):
        o = os.path.join(obj, 'gen_' + os.path.basename(f)[:-2] + '.o')
        if not os.path.exists(o) or newer(f, o) or newer(cpu_h, o):
            jobs.append([tc['cc']] + cflags + ['-c', f, '-o', o])
    for f in sorted(glob.glob(os.path.join(ROOT, 'runtime', '*.c'))):
        o = os.path.join(obj, 'rt_' + os.path.basename(f)[:-2] + '.o')
        jobs.append([tc['cc']] + cflags + ['-Wall', '-Wno-unused-function', '-c', f, '-o', o])
    for f in sorted(glob.glob(os.path.join(ROOT, 'runtime', '*.cpp'))):
        # the Streamline (DLSS) wrapper; NVIDIA's headers need the Windows 10 API level
        o = os.path.join(obj, 'rt_' + os.path.basename(f)[:-4] + '_cpp.o')
        cxx = [tc['cxx'], '-std=c++17', '-O1', '-g', '-Wall', '-Wno-unused-function', '-Wno-nontrivial-memaccess',
               '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000000', '-I' + os.path.join(ROOT, 'runtime')]
        cxx += ['-I' + os.path.join(tc['sl'], 'include')] if tc['sl'] else ['-DNO_DLSS']
        jobs.append(cxx + ['-c', f, '-o', o])
    print(f'[{name}] compiling {len(jobs)} files ...', flush=True)
    failed = False
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for code, text in pool.map(run, jobs):
            if text.strip():
                print(text.rstrip())
            failed |= code != 0
    if failed:
        die('compile errors (see above)')

    # 3. link: one self-contained exe, SDL linked statically
    exe = os.path.join(out, prog['exe'] + '.exe')
    link = [tc['cxx'], '-g', '-static', '-o', exe] + sorted(glob.glob(os.path.join(obj, '*.o'))) + \
           ['-L' + os.path.join(tc['sdl'], 'lib'), '-l:libSDL2.a'] + LIBS + \
           ['-mconsole' if os.environ.get('CONSOLE') else '-mwindows']
    code, text = run(link)
    if code:
        print(text.rstrip())
        die('the link failed.' + (' Is the program still running?' if 'ermission denied' in text else ''))

    # 4. dist/: the exe without debug information, and what it loads at run time
    dist = os.path.join(ROOT, 'dist')
    os.makedirs(dist, exist_ok=True)
    for _ in range(5):                      # a just-written exe can be briefly locked by a virus scanner
        code, _text = run([tc['strip'], '-o', os.path.join(dist, prog['exe'] + '.exe'), exe])
        if not code:
            break
        time.sleep(1)
    if name == 'tfx':
        # the controls window (tfx.exe --controls) as a double-click
        lines = ['@echo off', 'rem TFX controls: bind joystick, throttle and controller buttons and axes',
                 'rem (writes the joy... lines of tfx.ini next to tfx.exe).', 'cd /d "%~dp0"', 'start "" tfx.exe --controls', '']
        with open(os.path.join(dist, 'controls.bat'), 'wb') as f:
            f.write('\r\n'.join(lines).encode('ascii'))
        copies = []
        if tc['sl']:                        # NVIDIA Streamline + DLSS (signed), loaded when upscaler=dlss
            b = os.path.join(tc['sl'], 'bin', 'x64')
            copies += [(os.path.join(b, n), n) for n in ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll',
                                                         'sl.dlss_g.dll', 'nvngx_dlssg.dll', 'sl.reflex.dll', 'sl.pcl.dll')]
            copies += [(os.path.join(b, 'reflex.license.txt'), 'NVIDIA_Reflex_LICENSE.txt'),
                       (os.path.join(tc['sl'], 'license.txt'), 'NVIDIA_Streamline_LICENSE.txt'),
                       (os.path.join(b, 'nvngx_dlss.license.txt'), 'NVIDIA_DLSS_LICENSE.txt')]
        if tc['ffx']:                       # AMD FidelityFX (signed), loaded when upscaler=fsr; its licence goes with it
            copies += [(f, os.path.basename(f)) for f in glob.glob(os.path.join(tc['ffx'], 'bin', '*.dll'))]
            copies += [(os.path.join(tc['ffx'], 'license.md'), 'AMD_FidelityFX_LICENSE.md')]
        for s, n in copies:
            for d in (out, dist):
                t = os.path.join(d, n)
                if os.path.exists(s) and (not os.path.exists(t) or newer(s, t)):
                    try:
                        shutil.copy2(s, t)
                    except OSError:
                        pass
    print(f'[{name}] built {exe}  (release copy: dist\\{prog["exe"]}.exe)', flush=True)


def stamp_file_text(p):
    return open(p).read() if os.path.exists(p) else ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', nargs='?', default='all', choices=['tfx', 'config', 'all'])
    ap.add_argument('--game', help='folder of the installed game (holds TFX.DAT and CONFIG.EXE)')
    ap.add_argument('--regen', action='store_true', help='translate the game code again (after changing tools/)')
    ap.add_argument('--force', action='store_true', help='build from an unsupported version of the game files')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--check', action='store_true', help='only check that the game files are the supported version')
    a = ap.parse_args()
    if a.check:
        game = os.path.normpath(game_dir(a.game))
        good = True
        for which in ('TFX.DAT', 'CONFIG.EXE'):
            p = os.path.join(game, which)
            if not os.path.isfile(p):
                print(f'{which}: not in {game}')
                good &= which != 'TFX.DAT'
                continue
            ok, text = identify(p, which)
            print(('OK    ' if ok else 'WRONG ') + text.replace('\n', '\n      '))
            good &= ok or which == 'CONFIG.EXE'
        sys.exit(0 if good else 1)
    try:
        import capstone  # noqa: F401  (the recompiler's disassembler)
    except ImportError:
        die('the Python package "capstone" is missing.\nRun build.bat, or: python -m pip install capstone')
    tcdir = os.environ.get('TFX_TOOLCHAIN', os.path.join(ROOT, 'toolchain'))
    llvm = find_one(os.path.join(tcdir, f'llvm-mingw-*-{TARGET}', 'bin'), 'the compiler (llvm-mingw)')
    tc = dict(cc=os.path.join(llvm, f'{TARGET}-w64-mingw32-clang'), cxx=os.path.join(llvm, f'{TARGET}-w64-mingw32-clang++'),
              strip=os.path.join(llvm, f'{TARGET}-w64-mingw32-strip'),
              sdl=find_one(os.path.join(tcdir, 'SDL2-*', f'{TARGET}-w64-mingw32'), "SDL2's MinGW development package"),
              ffx=find_one(os.path.join(tcdir, 'fidelityfx-*'), 'FidelityFX SDK', required=False),
              sl=find_one(os.path.join(tcdir, 'streamline-*'), 'Streamline SDK', required=False))
    if TARGET != 'x86_64':
        tc['ffx'] = tc['sl'] = None         # both SDKs are 64-bit only
    game = os.path.normpath(game_dir(a.game))
    print(f'game folder: {game}')
    print('upscalers:   the port\'s own (taa)' + (', AMD FSR' if tc['ffx'] else '') + (', NVIDIA DLSS' if tc['sl'] else ''))
    t0 = time.time()
    for name in (['tfx', 'config'] if a.what == 'all' else [a.what]):
        if name == 'config' and a.what == 'all' and not a.force:     # the setup program is optional: skip it, don't fail
            p = os.path.join(game, 'CONFIG.EXE')
            if not os.path.isfile(p):
                print('[config] no CONFIG.EXE in the game folder: tfxconfig.exe skipped')
                continue
            ok, text = identify(p, 'CONFIG.EXE')
            if not ok:
                print('[config] tfxconfig.exe skipped (the game itself is built):\n  ' + text.replace('\n', '\n  '))
                continue
        build(name, game, a, tc)
    print(f'done in {time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
