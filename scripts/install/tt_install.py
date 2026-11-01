"""Helpers for scripts/install/tt-install.sh (standard library only, Python 3.8+).

    tt_install.py verify ISO TOOLS_DIR
    tt_install.py extract ISO TOOLS_DIR XEX_OUT FLASH_OUT
    tt_install.py sync-tree SRC DST [EXCLUDED_NAME...]
    tt_install.py keep-mtimes NEW_DIR OLD_DIR
    tt_install.py artwork ICO OUT_DIR
    tt_install.py steam-shortcut --name N --exe PATH --start-dir DIR --icon PNG [--art-dir DIR]
    tt_install.py steam-find --exe PATH --art-dir DIR
    tt_install.py quote TEXT

The ISO is only ever opened for reading. Nothing here downloads anything.
"""
import argparse
import binascii
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys
import zlib


def _tools(tools_dir):
    sys.path.insert(0, str(tools_dir))
    import disc_image
    import patch_menu
    return disc_image, patch_menu


def cmd_verify(args):
    disc_image, _ = _tools(args.tools)
    disc_image.read_inputs(args.iso)
    print('Supported retail ISO verified (Title ID 545407DF). The disc image is unchanged.')


def _write_if_changed(path, data):
    path = Path(path)
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.part')
    temp.write_bytes(data)
    temp.replace(path)
    return True


def cmd_extract(args):
    disc_image, patch_menu = _tools(args.tools)
    xex, flash = disc_image.read_inputs(args.iso)
    _write_if_changed(args.xex_out, xex)
    out = Path(args.flash_out)
    out.parent.mkdir(parents=True, exist_ok=True)
    temp = out.with_name('flash.extract.rpf')
    temp.write_bytes(flash)
    patch_menu.patch(str(temp))  # offline menu, intro timing and the PC SETTINGS entry
    temp.replace(out)
    print('Game executable and menu archive read from the ISO and verified; menu patched. ISO unchanged.')


def _same(a, b):
    try:
        if a.stat().st_size != b.stat().st_size:
            return False
    except FileNotFoundError:
        return False
    with a.open('rb') as fa, b.open('rb') as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def cmd_sync_tree(args):
    """Mirror SRC into DST, touching only files whose content changed (keeps builds incremental)."""
    src, dst, skip = Path(args.src), Path(args.dst), set(args.exclude)
    wanted, changed = set(), 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in skip and d != '__pycache__']
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        wanted.add(rel)
        for name in files:
            if name in skip or name.endswith('.pyc'):
                continue
            s, d = Path(root) / name, dst / rel / name
            wanted.add(rel / name)
            if not _same(s, d):
                temp = d.with_name(d.name + '.part')
                shutil.copyfile(s, temp)
                os.chmod(temp, s.stat().st_mode & 0o777)
                temp.replace(d)
                changed += 1
    for root, dirs, files in os.walk(dst, topdown=False):
        rel = Path(root).relative_to(dst)
        if rel.parts and rel.parts[0] in skip:
            continue
        for name in files:
            if rel / name not in wanted and name not in skip:
                (Path(root) / name).unlink()
        if rel not in wanted and rel.parts and rel.parts[0] not in skip:
            try:
                Path(root).rmdir()
            except OSError:
                pass
    print(f'{changed} source file(s) updated.')


def cmd_keep_mtimes(args):
    """Give regenerated files that did not change their old timestamps, so Ninja skips them."""
    new, old = Path(args.new), Path(args.old)
    kept = 0
    for path in new.rglob('*'):
        prev = old / path.relative_to(new)
        if path.is_file() and prev.is_file() and _same(path, prev):
            st = prev.stat()
            os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
            kept += 1
    print(f'{kept} generated file(s) unchanged.')


# --- PNG artwork from the project's own icon -------------------------------------------------

def _png_chunks(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos = 8
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        yield kind, data[pos + 8:pos + 8 + length]
        pos += 12 + length


def _png_decode(data):
    """Decode an 8-bit RGB/RGBA, non-interlaced PNG into (width, height, RGBA bytes)."""
    idat = b''
    for kind, body in _png_chunks(data):
        if kind == b'IHDR':
            w, h, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
    if depth != 8 or color not in (2, 6) or interlace:
        raise ValueError('unsupported PNG layout')
    bpp = 4 if color == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    out, prev = bytearray(), bytearray(stride)
    pos = 0
    for _ in range(h):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 255
            elif ftype == 2:
                line[i] = (line[i] + b) & 255
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if bpp == 4:
            out += line
        else:
            for i in range(0, stride, 3):
                out += line[i:i + 3] + b'\xff'
    return w, h, out


def _png_encode(w, h, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', binascii.crc32(kind + body))
    raw = b''.join(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def _canvas(w, h, icon, scale):
    """A dark gradient card with the icon centred (integer nearest-neighbour scale)."""
    iw, ih, pix = icon
    out = bytearray(w * h * 4)
    for y in range(h):
        t = y / max(h - 1, 1)
        r, g, b = int(18 + 14 * t), int(24 + 18 * t), int(38 + 30 * t)
        out[y * w * 4:(y + 1) * w * 4] = bytes((r, g, b, 255)) * w
    sw, sh = iw * scale, ih * scale
    ox, oy = (w - sw) // 2, (h - sh) // 2
    for y in range(sh):
        src_row = (y // scale) * iw * 4
        dst_row = ((oy + y) * w + ox) * 4
        for x in range(sw):
            s = src_row + (x // scale) * 4
            a = pix[s + 3]
            if not a:
                continue
            d = dst_row + x * 4
            for k in range(3):
                out[d + k] = (pix[s + k] * a + out[d + k] * (255 - a)) // 255
    return out


def cmd_artwork(args):
    data = Path(args.ico).read_bytes()
    count = struct.unpack_from('<HHH', data)[2]
    best = None
    for i in range(count):
        w, h, _, _, _, _, size, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        w, h = w or 256, h or 256
        if data[offset:offset + 4] == b'\x89PNG' and (best is None or w > best[0]):
            best = (w, data[offset:offset + size])
    if best is None:
        raise SystemExit('no PNG image inside the icon')
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'tabletennis.png').write_bytes(best[1])
    try:
        icon = _png_decode(best[1])
    except ValueError as error:
        print(f'Steam artwork skipped: {error}')
        return
    # Steam library images for a non-Steam shortcut: capsule (portrait), wide capsule, hero, logo.
    art = out / 'steam'
    art.mkdir(exist_ok=True)
    (art / 'logo.png').write_bytes(best[1])
    for name, w, h, scale in (('capsule.png', 600, 900, 2), ('wide.png', 920, 430, 1),
                              ('hero.png', 1920, 620, 2)):
        (art / name).write_bytes(_png_encode(w, h, _canvas(w, h, icon, scale)))
    print('Icon and Steam artwork created from the project icon.')


# --- Steam shortcuts.vdf (binary KeyValues) --------------------------------------------------

def _vdf_read(data, pos=0):
    result = {}
    while True:
        kind = data[pos]
        pos += 1
        if kind == 8:
            return result, pos
        end = data.index(b'\x00', pos)
        key = data[pos:end].decode('utf-8', 'surrogateescape')
        pos = end + 1
        if kind == 0:
            value, pos = _vdf_read(data, pos)
        elif kind == 1:
            end = data.index(b'\x00', pos)
            value = data[pos:end].decode('utf-8', 'surrogateescape')
            pos = end + 1
        elif kind == 2:
            value = ('i32', struct.unpack_from('<I', data, pos)[0])
            pos += 4
        elif kind == 7:
            value = ('u64', struct.unpack_from('<Q', data, pos)[0])
            pos += 8
        else:
            raise ValueError(f'unsupported shortcuts.vdf field type {kind}')
        result[key] = value


def _vdf_write(obj):
    out = bytearray()
    for key, value in obj.items():
        k = key.encode('utf-8', 'surrogateescape') + b'\x00'
        if isinstance(value, dict):
            out += b'\x00' + k + _vdf_write(value)
        elif isinstance(value, str):
            out += b'\x01' + k + value.encode('utf-8', 'surrogateescape') + b'\x00'
        elif value[0] == 'i32':
            out += b'\x02' + k + struct.pack('<I', value[1] & 0xFFFFFFFF)
        else:
            out += b'\x07' + k + struct.pack('<Q', value[1])
    return bytes(out + b'\x08')


def _steam_roots():
    home = Path.home()
    seen = []
    for p in (home / '.local/share/Steam', home / '.steam/steam', home / '.steam/root',
              home / '.var/app/com.valvesoftware.Steam/.local/share/Steam'):
        if (p / 'userdata').is_dir():
            real = p.resolve()
            if real not in seen:
                seen.append(real)
    return seen


def _steam_users(root):
    """The most recently signed-in account if loginusers.vdf says so, else every account."""
    users = [d for d in (root / 'userdata').iterdir() if d.name.isdigit() and d.name != '0']
    try:
        text = (root / 'config/loginusers.vdf').read_text(errors='replace')
        current = None
        for block in text.split('"7656')[1:]:
            sid = '7656' + block.split('"', 1)[0]
            if '"MostRecent"\t\t"1"' in block or '"mostrecent"\t\t"1"' in block.lower():
                current = str(int(sid) & 0xFFFFFFFF)
        if current and any(u.name == current for u in users):
            return [u for u in users if u.name == current]
    except (OSError, ValueError):
        pass
    return users


def _shortcut_appid(exe, name):
    return binascii.crc32((exe + name).encode()) | 0x80000000


def _copy_art(art_dir, grid, appid):
    if not art_dir or not (Path(art_dir) / 'capsule.png').is_file():
        return
    grid.mkdir(parents=True, exist_ok=True)
    for src, dst in (('capsule.png', f'{appid}p.png'), ('wide.png', f'{appid}.png'),
                     ('hero.png', f'{appid}_hero.png'), ('logo.png', f'{appid}_logo.png')):
        shutil.copyfile(Path(art_dir) / src, grid / dst)


def cmd_steam_shortcut(args):
    """Add or update the shortcut in shortcuts.vdf. Only call this while Steam is not running."""
    exe = f'"{args.exe}"'
    start = f'"{args.start_dir}"'
    roots = _steam_roots()
    if not roots:
        raise SystemExit('no Steam user data found (start Steam and sign in once)')
    done = 0
    for root in roots:
        for user in _steam_users(root):
            config = user / 'config'
            config.mkdir(exist_ok=True)
            path = config / 'shortcuts.vdf'
            if path.exists() and path.stat().st_size:
                data = path.read_bytes()
                top, _ = _vdf_read(data)
                # Refuse to rewrite a file we would not reproduce byte for byte.
                again = _vdf_write(top)
                if 'shortcuts' not in top or not (
                        data == again or (data.startswith(again) and set(data[len(again):]) <= {0, 8, 10})):
                    raise SystemExit(f'{path} has an unexpected layout; left unchanged')
                # The file as it was before the first change, plus the state before this one.
                first = path.with_name('shortcuts.vdf.before-tt-recomp')
                if not first.exists():
                    shutil.copy2(path, first)
                shutil.copy2(path, path.with_name('shortcuts.vdf.tt-backup'))
            else:
                top = {'shortcuts': {}}
            shortcuts = top['shortcuts']
            entry = None
            for value in shortcuts.values():
                if isinstance(value, dict) and value.get('Exe', value.get('exe')) == exe:
                    entry = value
            appid = _shortcut_appid(exe, args.name)
            if entry is None:
                entry = {'appid': ('i32', appid), 'AppName': '', 'Exe': '', 'StartDir': '',
                         'icon': '', 'ShortcutPath': '', 'LaunchOptions': '',
                         'IsHidden': ('i32', 0), 'AllowDesktopConfig': ('i32', 1),
                         'AllowOverlay': ('i32', 1), 'OpenVR': ('i32', 0), 'Devkit': ('i32', 0),
                         'DevkitGameID': '', 'DevkitOverrideAppID': ('i32', 0),
                         'LastPlayTime': ('i32', 0), 'FlatpakAppID': '', 'tags': {}}
                index = 0
                while str(index) in shortcuts:
                    index += 1
                shortcuts[str(index)] = entry
            else:
                appid = entry.get('appid', ('i32', appid))[1]
            entry.update({'AppName': args.name, 'Exe': exe, 'StartDir': start, 'icon': args.icon})
            temp = path.with_name('shortcuts.vdf.tt-new')
            payload = _vdf_write(top)
            _vdf_read(payload)  # round-trip check before replacing the real file
            temp.write_bytes(payload)
            temp.replace(path)
            _copy_art(args.art_dir, config / 'grid', appid)
            done += 1
            print(f'Steam shortcut "{args.name}" written for Steam account {user.name}.')
    if not done:
        raise SystemExit('no Steam account found in userdata')


def cmd_steam_find(args):
    """After Steam added the shortcut itself, find its id and add the artwork."""
    found = False
    for root in _steam_roots():
        for user in _steam_users(root):
            path = user / 'config/shortcuts.vdf'
            try:
                top, _ = _vdf_read(path.read_bytes())
            except (OSError, ValueError, IndexError):
                continue
            for value in top.get('shortcuts', {}).values():
                exe = value.get('Exe', value.get('exe', '')) if isinstance(value, dict) else ''
                if args.exe in exe:
                    appid = value.get('appid', ('i32', 0))[1]
                    if appid:
                        _copy_art(args.art_dir, user / 'config/grid', appid)
                        found = True
    print('Steam artwork added.' if found else 'Shortcut not in shortcuts.vdf yet.')
    return 0 if found else 1


def cmd_quote(args):
    from urllib.parse import quote
    print(quote(args.text, safe=''))


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('verify'); s.add_argument('iso'); s.add_argument('tools')
    s = sub.add_parser('extract')
    for a in ('iso', 'tools', 'xex_out', 'flash_out'):
        s.add_argument(a)
    s = sub.add_parser('sync-tree'); s.add_argument('src'); s.add_argument('dst'); s.add_argument('exclude', nargs='*')
    s = sub.add_parser('keep-mtimes'); s.add_argument('new'); s.add_argument('old')
    s = sub.add_parser('artwork'); s.add_argument('ico'); s.add_argument('out')
    s = sub.add_parser('steam-shortcut')
    for a in ('--name', '--exe', '--start-dir', '--icon'):
        s.add_argument(a, required=True)
    s.add_argument('--art-dir')
    s = sub.add_parser('steam-find'); s.add_argument('--exe', required=True); s.add_argument('--art-dir')
    s = sub.add_parser('quote'); s.add_argument('text')
    args = p.parse_args()
    try:
        return globals()['cmd_' + args.cmd.replace('-', '_')](args) or 0
    except (ValueError, FileNotFoundError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
