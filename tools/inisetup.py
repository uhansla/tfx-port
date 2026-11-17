"""Ask for the port's options one by one and write them to tfx.ini.

    python tools/inisetup.py <folder with tfx.exe>

build.bat runs it after copying tfx.exe into the game folder.  Each option is
shown with its choices and what each one does; Enter keeps the current value.
Only the lines of options you change are rewritten, in place; the rest of
tfx.ini (comments, joystick bindings) stays as it is.  A missing tfx.ini is
first written with the defaults and comments that tfx.exe would write.

The options, their choices and their descriptions are read from the table of
the controls window's Settings page (runtime/joyui.c), and the default file
from runtime/settings.c, so all three always agree.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADING = '; set when building (build.bat); controls.bat has the same options'
STR = r'"((?:[^"\\]|\\.)*)"'


def c_string(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def read_options():
    """opts[] and opt_group[] of runtime/joyui.c -> [(group, key, label, default, need, [(label, value, desc)])]"""
    src = open(os.path.join(ROOT, 'runtime', 'joyui.c'), encoding='utf-8').read()
    body = re.search(r'static const struct option opts\[\] = \{(.*?)\n\};', src, re.S)
    groups = re.search(r'opt_group\[NOPT\] = \{(.*?)\};', src, re.S)
    if not body or not groups:
        raise ValueError('the option table in runtime/joyui.c has changed shape')
    gl = [None if g.strip() == '0' else c_string(g.strip()[1:-1]) for g in re.findall(r'"[^"]*"|\b0\b', groups.group(1))]
    opts = []
    for line in body.group(1).splitlines():
        m = re.match(r'\s*\{' + STR + ', ' + STR + ', ' + STR + r', (\d), \{', line)
        if m:
            opts.append([None, m.group(1), c_string(m.group(2)), c_string(m.group(3)), int(m.group(4)), []])
        for c in re.finditer(r'\{' + STR + ', ' + STR + ', ' + STR + r'\}', line):
            opts[-1][5].append(tuple(c_string(x) for x in c.groups()))
    for i, o in enumerate(opts):
        o[0] = gl[i] if i < len(gl) else None
    if not opts or any(not o[5] for o in opts):
        raise ValueError('could not read the option table in runtime/joyui.c')
    return opts


def default_ini():
    """the text settings.c writes to a new tfx.ini"""
    src = open(os.path.join(ROOT, 'runtime', 'settings.c'), encoding='utf-8').read()
    m = re.search(r'static const char \*default_ini =(.*?);\n', src, re.S)
    return ''.join(c_string(s) for s in re.findall(STR, m.group(1)))


def canon(v):
    """what a value means: on, 1 and auto are the same switch, as settings.c reads them"""
    v = v.strip().lower().replace(' ', '')
    if v in ('1', 'on', 'auto', '2x', 'yes'):
        return 'on'
    if v in ('0', 'off', 'no'):
        return 'off'
    return v


def ini_values(lines, keys):
    val = {}
    for line in lines:
        p = line.strip()
        if not p or p[0] in ';#' or '=' not in p:
            continue
        k, v = p.split('=', 1)
        if k.strip().lower() in keys:
            val[k.strip().lower()] = v.strip()          # the last one counts, as in settings.c
    return val


def ask(prompt):
    try:
        return input(prompt).strip()
    except EOFError:
        return None


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    folder = os.path.abspath(sys.argv[1])
    path = os.path.join(folder, 'tfx.ini')
    try:
        opts = read_options()
    except (OSError, ValueError) as e:
        sys.exit(f'settings: {e}; use controls.bat (Settings page) instead.')
    if os.path.exists(path):
        raw = open(path, 'rb').read().decode('latin-1')
        new_file = False
    else:
        raw = default_ini().replace('\n', '\r\n')
        new_file = True
    nl = '\r\n' if '\r\n' in raw else '\n'
    lines = raw.split(nl)
    keys = {o[1] for o in opts}
    cur = {o[1]: o[3] for o in opts}
    cur.update(ini_values(lines, keys))
    has = lambda n: os.path.exists(os.path.join(folder, n))
    has_fsr = has('amd_fidelityfx_loader_dx12.dll') and has('amd_fidelityfx_upscaler_dx12.dll')
    has_dlss = has('sl.interposer.dll')
    has_art = has(os.path.join('art', 'remacri', 'pack.txt'))

    print(f'Settings of the port ({path}{", new" if new_file else ""})')
    print('For each option: the number of a choice, or Enter to keep the one marked with >.')
    print('s skips the remaining options; x leaves without saving anything.')
    new = dict(cur)
    is_ = lambda k, v: canon(new[k]) == canon(v)
    stop = False
    for group, key, label, _def, need, choices in opts:
        if stop:
            break
        if group:
            print(f'\n== {group} ==')
        why = None
        if need >= 1 and not is_('renderer', 'native'):
            why = 'used with the native renderer'
        elif need == 2 and not is_('widescreen', 'on'):
            why = 'used when Widescreen is on'
        elif need == 3 and is_('upscaler', 'off'):
            why = 'used with an upscaler'
        if why:
            print(f'\n{label}: {why}, so it is not asked (now: {new[key] or "default"}).')
            continue
        sel = next((i for i, c in enumerate(choices) if canon(c[1]) == canon(new[key])), None)
        print(f'\n{label}  ({key}=)')
        w = max(len(c[0]) for c in choices)
        for i, (cl, cv, desc) in enumerate(choices):
            note = ''
            if key == 'upscaler' and cv == 'fsr' and not has_fsr:
                note = " AMD's DLLs aren't next to tfx.exe (build.bat -Upscalers), so TAA is used instead."
            elif key == 'upscaler' and cv == 'dlss' and not has_dlss:
                note = " NVIDIA's DLLs aren't next to tfx.exe (build.bat -Upscalers), so TAA is used instead."
            elif key == 'ui_filter' and cv == 'remacri' and not has_art:
                note = ' There is no art pack next to tfx.exe, so it works as MMPX.'
            elif key == 'frame_generation' and i > 0 and not has_dlss:
                note = " NVIDIA's DLLs aren't next to tfx.exe, so it stays off."
            elif key == 'renderer' and cv == 'native' and is_('presenter', 'sdl'):
                note = ' The presenter is SDL: choose Direct3D 12 for it.'
            print(f' {">" if i == sel else " "} {i + 1}  {cl:<{w}}  {desc}{note}')
        if sel is None:
            print(f' > -  {new[key]}  (as written in tfx.ini)')
        if key == 'window':
            print('       Another size: type it, e.g. 1500x1000')
        keep = choices[sel][0] if sel is not None else new[key]
        while True:
            a = ask(f'  {label} [Enter = {keep}]: ')
            if a is None:                                   # no more input: keep the rest
                stop = True
                break
            al = a.lower()
            if al == '':
                break
            if al == 's':
                stop = True
                break
            if al == 'x':
                print('Nothing was saved.')
                return
            if al.isdigit() and 1 <= int(al) <= len(choices):
                new[key] = choices[int(al) - 1][1]
                break
            hit = [c for c in choices if al in (c[0].lower(), c[1].lower())]
            if hit:
                new[key] = hit[0][1]
                break
            if key == 'window' and re.fullmatch(r'\d{3,5}x\d{3,5}', al):
                new[key] = al
                break
            print(f'  Type a number from 1 to {len(choices)}, or press Enter.')

    changed = [o for o in opts if new[o[1]] != cur[o[1]]]
    if not changed and not new_file:
        print('\nNothing has changed; tfx.ini is as it was.')
        return
    if changed:
        print('\nChanged:')
        for o in changed:
            print(f'  {o[1]}={new[o[1]]}' if new[o[1]] else f'  {o[1]}: back to the default')
    if ask(f'Save to {path}? [Y/n] ') in ('n', 'N'):
        print('Nothing was saved.')
        return

    # the changed lines in place (every copy of the key, so the last one is right too);
    # a default with no value (joy_deadzone) removes the line
    out, done = [], set()
    for line in lines:
        p = line.strip()
        k = p.split('=', 1)[0].strip().lower() if p and p[0] not in ';#' and '=' in p else None
        o = next((o for o in changed if o[1] == k), None)
        if o:
            done.add(k)
            if new[k]:
                out.append(f'{k}={new[k]}')
            elif out and out[-1] == HEADING:              # our own heading goes with the line
                out.pop()
            continue
        out.append(line)
    extra = [f'{o[1]}={new[o[1]]}' for o in changed if o[1] not in done and new[o[1]]]
    if extra:
        while out and out[-1] == '':
            out.pop()
        out += ['', HEADING] + extra + ['']
    tmp = path + '.new'
    with open(tmp, 'wb') as f:
        f.write(nl.join(out).encode('latin-1'))
    os.replace(tmp, path)
    print(f'Saved. controls.bat (Settings page) changes them later; the comments in tfx.ini explain every option.')


if __name__ == '__main__':
    main()
