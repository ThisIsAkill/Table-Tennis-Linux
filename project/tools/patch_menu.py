"""Locally create the offline/startup menu override; no original archive bundled."""
import hashlib
from pathlib import Path
import struct
import zlib

ORIGINAL_ARCHIVE = '2dd3b2e2141caf554a63b0791271ab256b9597238f3685ca9569776fb80e28bc'
ORIGINAL_MOVIE = '75c3a8250db52a44461b5fea383c148ce938229231257f5cd7f8a985848dfa17'
OFFLINE_MOVIE = '220d6f63f77b57f90b474d2f8fe33e2b077347e087160b49a98a2afd8214f13a'
# Offline menu with PC SETTINGS; the main menu's SYSTEM LINK entry is added on top of it.
PC_SETTINGS_MOVIE = 'ebab047a621e51954a1bfc7c81aa36e3e6c996066453c04c256874d32e237513'
# With SYSTEM LINK in the main menu (release 1.1.0); ONLINE is added on top of it.
SYSTEM_LINK_MOVIE = '2d41824ecc440ebb9f235b426045cb9f38e2838bd4cc2700e572b55b6026d546'
# With ONLINE above SYSTEM LINK (release 1.2.0); QUIT GAME is added on top of it.
ONLINE_MOVIE = 'eaab6d629629044903a565b63156823948b8d8f3f7bb9d3409b70fd62c8bbe02'
PATCHED_MOVIE = '3ea158ad418717bcd2e7eaf13529a937b14821720618b0998cc54dcd54b83fd0'
# Earlier patch with PC SETTINGS as the last OPTIONS item; its regions are rewritten in full.
PC_SETTINGS_LAST_MOVIE = 'cb3f3eaf65f096ce66e783b364aebd9ca317d7a46a250cf841cda3efa6cde301'
# In-match pause menu (pongpause_001.xeck): PC SETTINGS below AUDIO OPTIONS in its OPTIONS list.
ORIGINAL_PAUSE_MOVIE = '7b6f9e70c58adcea5974681aad4dd2fa7915dd6f54015efdaaf888818ba0861d'
PATCHED_PAUSE_MOVIE = 'ca15a4b5931015c5547ee019ee59febb82bf62b7a547d7c0353f104d3db7d3d4'


# Small AVM1 assembler for the PC SETTINGS entry. Constant pool indices refer
# to the pools already present in the movie's action blocks.
def _int(value):
    return b'\x07' + struct.pack('<i', value)


def _pool(index):
    return (b'\x08' + bytes([index])) if index < 256 else (b'\x09' + struct.pack('<H', index))


def _reg(index):
    return b'\x04' + bytes([index])


def _str(text):
    return b'\x00' + text.encode() + b'\0'


def _push(*items):
    body = b''.join(items)
    return b'\x96' + struct.pack('<H', len(body)) + body


def _branch(opcode, offset):
    return bytes([opcode]) + b'\x02\x00' + struct.pack('<h', offset)


GET_VARIABLE, GET_MEMBER, SET_MEMBER, NEW_OBJECT, STRICT_EQUALS = b'\x1c', b'\x4e', b'\x4f', b'\x40', b'\x66'
GREATER, NOT, EQUALS2, POP = b'\x67', b'\x12', b'\x49', b'\x17'


def _store_register(index):
    return b'\x87\x01\x00' + bytes([index])
JUMP, IF = 0x99, 0x9D


def _fit(code, size):
    """Pad a rewritten region to its original size with a Jump over zeros. A gap too small for
    the Jump (2 or 4 bytes) is filled with PushDuplicate + Pop pairs, which leave the stack as
    it was."""
    gap = size - len(code)
    if gap in (2, 4):
        return code + b'\x4c\x17' * (gap // 2)
    if gap < 0 or 0 < gap < 5:
        raise ValueError('PC settings patch does not fit')
    return code + (_branch(JUMP, gap - 5) + bytes(gap - 5) if gap else b'')


def _options_list(size):
    """_global.LIST_OPTIONS with ["PC SETTINGS", FE_GAME_OPTIONS, -1] added as the third item,
    after AUDIO OPTIONS.

    Each item is [text, target frame state, -1]; Array pops its arguments, so
    the last item is pushed first. The frame state constants (FE_GAME_OPTIONS
    21, FE_CONTROLLER 79, FE_AUDIO_OPTIONS 24) are inlined to make room. The
    PC SETTINGS state is never used: the accept handler raises a flag for it.
    """
    # In menu order; None marks PC SETTINGS. The texts are the movie's own t_* constants.
    items = [(21, 286), (24, 285), None, (79, 284), (21, 283), (21, 282), (21, 281), (21, 280)]
    code = _push(_pool(279))
    for item in reversed(items):
        if item is None:
            code += _push(_int(-1), _int(21), _str('PC SETTINGS'), _int(3), _pool(162)) + NEW_OBJECT
            continue
        state, text = item
        code += (_push(_int(-1), _int(state), _pool(1)) + GET_VARIABLE + _push(_pool(text)) + GET_MEMBER +
                 _push(_int(3), _pool(162)) + NEW_OBJECT)
    code += _push(_int(8), _pool(162)) + NEW_OBJECT
    return _fit(code, size)


def _options_accept(start, end, done):
    """OPTIONS accept handler: item 2 (PC SETTINGS) raises _global.GLOBAL_PC_SETTINGS,
    which the executable polls to open its page; items 4-7 raise the save/load flags.
    Every other item falls through to the game's own handling (its target frame state)."""
    flags = [_pool(214), _pool(215), _pool(216), _pool(217), _str('GLOBAL_PC_SETTINGS')]
    bodies = [_push(_reg(1), flag, _int(1)) + SET_MEMBER for flag in flags]
    sound = (_push(_reg(1), _pool(81), _reg(1), _pool(213)) + GET_MEMBER + SET_MEMBER +
             _push(_reg(1), _pool(83), _int(1)) + SET_MEMBER)
    # The first test compares the item index StoreRegister left on the stack.
    tests = [_push(_int(2)) + STRICT_EQUALS] + [_push(_reg(0), _int(i)) + STRICT_EQUALS for i in (4, 5, 6, 7)]
    # Test i jumps to body test_body[i]: index 2 -> the PC flag (body 4), 4-7 -> bodies 0-3.
    test_body = [4, 0, 1, 2, 3]
    position = start + sum(len(test) + 5 for test in tests) + 5
    targets = []
    for body in bodies[:4]:
        targets.append(position)
        position += len(body) + 5
    targets.append(position)
    sound_at = position + len(bodies[4])
    code = b''
    for test, body in zip(tests, test_body):
        target = targets[body]
        code += test
        code += _branch(IF, target - (start + len(code) + 5))
    code += _branch(JUMP, end - (start + len(code) + 5))
    for body in bodies[:4]:
        code += body
        code += _branch(JUMP, sound_at - (start + len(code) + 5))
    code += bodies[4] + sound
    code += _branch(JUMP, done - (start + len(code) + 5))
    return _fit(code, end - start)


def _add_pc_settings(movie):
    movie[0x1878F:0x188BC] = _options_list(0x188BC - 0x1878F)
    movie[0x13C0D:0x13D00] = _options_accept(0x13C0D, 0x13D00, 0x13E3C)
    # The localisation script copies the translated labels into LIST_OPTIONS by row
    # (LIST_OPTIONS[i][0] = t_...); move CONTROLS through LOAD SHARED SETTINGS down one row
    # so PC SETTINGS keeps its own label in row 2.
    for address, row in ((0x3AC880, 2), (0x3AC8AB, 3), (0x3AC8D6, 4), (0x3AC901, 5), (0x3AC92C, 6)):
        if movie[address:address + 4] != b'\x96\x05\x00\x07' or movie[address + 4] not in (row, row + 1):
            raise ValueError('Unexpected OPTIONS label code')
        movie[address + 4] = row + 1


def _add_system_link(movie):
    """Put the main menu's online entry (removed by the offline patch) back as SYSTEM LINK.

    The item is [text, FE_WAIT_FOR_ONLINE (91), -1] in row 3, between TRAINING MODE and OPTIONS,
    where the original menu has Xbox LIVE. The executable runs the game's online front end over
    the local network when system link is enabled and keeps it blocked otherwise.
    """
    if movie[0x17F0D:0x17F10] != b'\x99\x02\x00' or movie[0x17FE5] != 5:
        raise ValueError('Unexpected main menu list code')
    movie[0x17F0D:0x17F35] = _fit(_push(_int(-1), _int(91), _str('SYSTEM LINK'), _int(3), _pool(162)) +
                                  NEW_OBJECT, 0x17F35 - 0x17F0D)
    movie[0x17FE5] = 6
    # The localisation script copies the translated labels into LIST_MAIN by row: give row 3
    # its label back and move OPTIONS and EXTRAS to their original rows.
    label = (_push(_pool(0)) + GET_VARIABLE + _push(_pool(2)) + GET_MEMBER + _push(_int(3)) + GET_MEMBER +
             _push(_int(0), _str('SYSTEM LINK')) + SET_MEMBER)
    movie[0x3AC212:0x3AC23D] = _fit(label, 0x3AC23D - 0x3AC212)
    for address, row in ((0x3AC249, 3), (0x3AC274, 4)):
        if movie[address:address + 4] != b'\x96\x05\x00\x07' or movie[address + 4] != row:
            raise ValueError('Unexpected main menu label code')
        movie[address + 4] = row + 1


def _main_list_item(state, label):
    """[_global.<label>, state, -1] with the frame state inlined."""
    return (_push(_int(-1), _int(state), _pool(1)) + GET_VARIABLE + _push(_pool(label)) + GET_MEMBER +
            _push(_int(3), _pool(162)) + NEW_OBJECT)


MAIN_LIST_START, MAIN_LIST_END = 0x17EBB, 0x17FEC


def _main_list_literal(text):
    """[text, FE_WAIT_FOR_ONLINE (91), -1]: the rows the executable tells apart by row number."""
    return _push(_int(-1), _int(91), _str(text), _int(3), _pool(162)) + NEW_OBJECT


def _main_list_head():
    """The 1.2.0 list's items pushed before EXHIBITION (the list is pushed last item first):
    EXTRAS (sharing its Push with the list's name), OPTIONS, SYSTEM LINK, ONLINE, TRAINING MODE."""
    return (_push(_pool(210), _int(-1), _int(85), _pool(1)) + GET_VARIABLE + _push(_pool(211)) +
            GET_MEMBER + _push(_int(3), _pool(162)) + NEW_OBJECT + _main_list_item(18, 212) +
            _main_list_literal('SYSTEM LINK') + _main_list_literal('ONLINE') + _main_list_item(83, 214))


def _compact_main_list(exhibition):
    """The 8-row LIST_MAIN with QUIT GAME, written compactly to fit the original space. The
    menu's label script (the region _add_online edits) sets the text of rows 0-3, 5 and 6 from
    the localisation strings after the list is built, so those items carry an empty text here;
    SYSTEM LINK (4) and QUIT GAME (7) keep theirs. The game's Flash player only builds arrays
    with new Array(...) (InitArray left the menu empty). Items are [text, target state, -1],
    pushed last element first; EXHIBITION is the original item."""

    def item(state, text=''):
        return _push(_int(-1), _int(state), _str(text), _int(3), _pool(162)) + NEW_OBJECT

    # The list's name shares the first Push with the QUIT GAME item (FE_WAIT_FOR_ONLINE, 91).
    quit_item = _push(_pool(210), _int(-1), _int(91), _str('QUIT GAME'), _int(3), _pool(162)) + NEW_OBJECT
    return (quit_item + item(85) + item(18) + item(91, 'SYSTEM LINK') + item(91) + item(83) +
            exhibition + item(5) + _push(_int(8), _pool(162)) + NEW_OBJECT)


def _main_list(exhibition, quit):
    """LIST_MAIN: TOURNAMENT, EXHIBITION, TRAINING MODE, ONLINE (row 3), SYSTEM LINK (row 4),
    OPTIONS, EXTRAS and, with quit, QUIT GAME (row 7). Frame states are inlined
    (FE_TOURNAMENT 5, FE_TRAINING 83, FE_OPTIONS 18, FE_EXTRAS 85); EXHIBITION is the original item."""
    if quit:
        return _compact_main_list(exhibition)
    return (_main_list_head() + exhibition + _main_list_item(5, 220) +
            _push(_int(7), _pool(162)) + NEW_OBJECT)


def _add_quit(movie):
    """Main menu: QUIT GAME as the last row (7), after an idea from davidptrssvea's
    Table-Tennis-Rockstar-PC v1.1.0. It opens nothing itself: like ONLINE and SYSTEM LINK it has
    FE_WAIT_FOR_ONLINE as its target, so the accept handler leaves row 7 in iCurrChooseModeSel,
    and the executable closes the game when it reads that row (system_link.cpp)."""
    offset = MAIN_LIST_START + len(_main_list_head())
    exhibition = bytes(movie[offset:offset + 0x17FB9 - 0x17F5D])
    if exhibition[-1:] != NEW_OBJECT or bytes(movie[MAIN_LIST_START:offset]) != _main_list_head():
        raise ValueError('Unexpected main menu list code')
    movie[MAIN_LIST_START:MAIN_LIST_END] = _fit(_main_list(exhibition, quit=True), MAIN_LIST_END - MAIN_LIST_START)


def _add_online(movie):
    """Main menu: ONLINE in the original Xbox LIVE row (3) and SYSTEM LINK below it (row 4).

    Both open the game's online menus (FE_WAIT_FOR_ONLINE, 91); the executable reads which one
    was chosen (see the accept handler below) to play over the online server or the local
    network.
    LIST_MAIN is rebuilt with the frame states inlined (FE_TOURNAMENT 5, FE_TRAINING 83,
    FE_OPTIONS 18, FE_EXTRAS 85) to make room; the EXHIBITION item is kept as it is.
    """
    exhibition = bytes(movie[0x17F5D:0x17FB9])
    if movie[0x17F0D:0x17F12] != b'\x96\x1e\x00\x07\xff' or exhibition[-1:] != NEW_OBJECT:
        raise ValueError('Unexpected main menu list code')
    movie[MAIN_LIST_START:MAIN_LIST_END] = _fit(_main_list(exhibition, quit=False), MAIN_LIST_END - MAIN_LIST_START)
    # The localisation script's row 3 label becomes ONLINE; OPTIONS and EXTRAS move down a row.
    label = (_push(_pool(0)) + GET_VARIABLE + _push(_pool(2)) + GET_MEMBER + _push(_int(3)) + GET_MEMBER +
             _push(_int(0), _str('ONLINE')) + SET_MEMBER)
    movie[0x3AC212:0x3AC23D] = _fit(label, 0x3AC23D - 0x3AC212)
    for address, row in ((0x3AC249, 4), (0x3AC274, 5)):
        if movie[address:address + 4] != b'\x96\x05\x00\x07' or movie[address + 4] != row:
            raise ValueError('Unexpected main menu label code')
        movie[address + 4] = row + 1
    # Main menu accept handler: the game asks the executable for its online menu
    # (initRequestOnline) only for row 3. Ask for both rows that open it (target state 91), and
    # leave the chosen row in iCurrChooseModeSel, which the executable reads and clears.
    # r1 = _global; pool: 21 target_STATE_NEW, 218 initRequestOnline, 28 iCurrChooseModeSel,
    # 30 MENU_SELECTIONS, 31 MENU_ID.
    start, end = 0x13F9C, 0x13FE2
    if movie[start:start + 7] != _push(_reg(1), _pool(14)):
        raise ValueError('Unexpected main menu accept code')
    request = _push(_reg(1), _pool(218), _int(1)) + SET_MEMBER
    # (ToInteger: the executable reads the variable as an integer.)
    row = (_push(_reg(1), _pool(28), _reg(1), _pool(30)) + GET_MEMBER + _push(_reg(1), _pool(31)) + GET_MEMBER +
           GET_MEMBER + b'\x18' + SET_MEMBER)
    test = _push(_reg(1), _pool(21)) + GET_MEMBER + _push(_int(91)) + EQUALS2 + NOT
    code = test + _branch(IF, len(request) + len(row)) + request + row
    movie[start:end] = _fit(code, end - start)


def _pause_options_list(size):
    """_global.LIST_iPAUSE_OPTIONS with ["PC SETTINGS", iPAUSE_OPTIONS, -1] as the third item,
    after AUDIO OPTIONS.

    Items are [text, target state, -1] (pause states: CONTROLLER 3, AUDIO 2, GAME 7, HELPTIPS 22,
    OPTIONS 1), pushed last item first. Targeting OPTIONS itself makes the accept handler treat
    the item as "same state", where it raises the PC flag. GAME OPTIONS and HELP TIPS read their
    labels with GetVariable (resolved through _global, as the movie's own functions do) to make
    room; the label script rewrites every row's text by index anyway.
    """
    items = [(3, 22, False), (2, 20, False), None, (7, 18, True), (22, 16, True)]
    code = b''
    head = [_pool(14)]
    for item in reversed(items):
        if item is None:
            code += _push(*head, _int(-1), _int(1), _str('PC SETTINGS'), _int(3), _pool(4)) + NEW_OBJECT
        else:
            state, text, short = item
            if short:
                code += _push(*head, _int(-1), _int(state), _pool(text)) + GET_VARIABLE
            else:
                code += (_push(*head, _int(-1), _int(state), _pool(0)) + GET_VARIABLE + _push(_pool(text)) +
                         GET_MEMBER)
            code += _push(_int(3), _pool(4)) + NEW_OBJECT
        head = []
    code += _push(_int(5), _pool(4)) + NEW_OBJECT
    return _fit(code, size)


def _pause_accept(start, end, done):
    """Pause menu A handler for the highlighted item (r0 = item, r1 = _global):
    if item[2] > -2: if item[1] != STATE_CURRENT, go to item[1] (the game's behaviour);
    otherwise raise GLOBAL_PC_SETTINGS in the OPTIONS list, or play ERROR anywhere else."""
    item = (_push(_pool(52)) + GET_VARIABLE + _push(_reg(1), _pool(20)) + GET_MEMBER + _push(_reg(1), _pool(53)) +
            GET_MEMBER + GET_MEMBER + GET_MEMBER + _store_register(0) + POP)
    disabled = _push(_reg(0), _int(2)) + GET_MEMBER + _push(_int(-2)) + GREATER + NOT
    same_state = _push(_reg(1), _pool(2)) + GET_MEMBER + _push(_reg(0), _int(1)) + GET_MEMBER + EQUALS2
    go = (_push(_reg(1), _pool(10), _reg(0), _int(1)) + GET_MEMBER + SET_MEMBER +
          _push(_reg(1), _pool(23), _reg(1), _pool(24)) + GET_MEMBER + SET_MEMBER)
    not_options = _push(_reg(1), _pool(2)) + GET_MEMBER + _push(_reg(1), _pool(3)) + GET_MEMBER + EQUALS2 + NOT
    flag = _push(_reg(1), _str('GLOBAL_PC_SETTINGS'), _int(1)) + SET_MEMBER
    camera = _push(_reg(1), _pool(71), _reg(1), _pool(72)) + GET_MEMBER + SET_MEMBER
    error = _push(_reg(1), _pool(71), _reg(1), _pool(74)) + GET_MEMBER + SET_MEMBER
    sound = _push(_reg(1), _pool(73), _int(1)) + SET_MEMBER
    # Branches are (opcode, label); strings starting with ':' define labels.
    parts = [item, disabled, (IF, 'error'), same_state, (IF, 'same'), go, (JUMP, 'camera'),
             ':same', not_options, (IF, 'error'), flag,
             ':camera', camera, (JUMP, 'sound'), ':error', error, ':sound', sound, (JUMP, 'done')]
    labels = {'done': done}
    position = start
    for part in parts:
        if isinstance(part, str):
            labels[part[1:]] = position
        else:
            position += 5 if isinstance(part, tuple) else len(part)
    code = b''
    for part in parts:
        if isinstance(part, tuple):
            code += _branch(part[0], labels[part[1]] - (start + len(code) + 5))
        elif not isinstance(part, str):
            code += part
    return _fit(code, end - start)


def _add_pause_pc_settings(movie):
    movie[0x7A83:0x7B30] = _pause_options_list(0x7B30 - 0x7A83)
    movie[0x4C96:0x4D87] = _pause_accept(0x4C96, 0x4D87, 0x531B)
    # The label script copies the translated labels into LIST_iPAUSE_OPTIONS by row; move
    # GAME OPTIONS and HELP TIPS down one row so PC SETTINGS keeps its own label in row 2.
    for address, row in ((0x9096, 2), (0x90C1, 3)):
        if movie[address:address + 4] != b'\x96\x05\x00\x07' or movie[address + 4] != row:
            raise ValueError('Unexpected pause OPTIONS label code')
        movie[address + 4] = row + 1


def _read_archive(path):
    archive = bytearray(path.read_bytes())
    if archive[:4] != b'RPF0':
        raise ValueError('Not an RPF0 menu archive')
    toc, count = struct.unpack_from('<II', archive, 4)
    names = toc + count * 16
    if not 0 < count < 100000 or toc < 12 or names >= len(archive):
        raise ValueError('Invalid archive table')
    entries = []
    for i in range(count):
        entry = toc + i * 16
        name, offset, packed, raw_size = struct.unpack_from('<4I', archive, entry)
        if name & 0x80000000:
            continue
        end = archive.index(0, names + name)
        if offset + packed > len(archive):
            raise ValueError('Archive entry outside file')
        entries.append((archive[names + name:end], entry, offset, packed, raw_size))
    return archive, entries


def _read_movie(archive, entries, name):
    matches = [e for e in entries if e[0] == name]
    if len(matches) != 1:
        raise ValueError('Expected exactly one ' + name.decode())
    _, entry, offset, packed, raw_size = matches[0]
    if raw_size > 16 * 1024 * 1024:
        raise ValueError('Unexpected movie size')
    decoder = zlib.decompressobj(-15)
    movie = bytearray(decoder.decompress(archive[offset:offset + packed], raw_size + 1))
    if len(movie) != raw_size or not decoder.eof:
        raise ValueError('Invalid movie compression')
    return movie, entry, offset


def patch(path):
    path = Path(path)
    archive, entries = _read_archive(path)
    original_archive = hashlib.sha256(archive).hexdigest() == ORIGINAL_ARCHIVE
    changed = False

    movie, entry, offset = _read_movie(archive, entries, b'pongui_001.xeck')
    current = hashlib.sha256(movie).hexdigest()
    if current != PATCHED_MOVIE:
        if current in (OFFLINE_MOVIE, PC_SETTINGS_LAST_MOVIE):
            _add_pc_settings(movie)
        elif original_archive and current == ORIGINAL_MOVIE:
            _patch_offline(movie)
            _add_pc_settings(movie)
        elif current not in (PC_SETTINGS_MOVIE, SYSTEM_LINK_MOVIE, ONLINE_MOVIE):
            raise ValueError('Unsupported menu archive; no changes made')
        if current not in (SYSTEM_LINK_MOVIE, ONLINE_MOVIE):
            if hashlib.sha256(movie).hexdigest() != PC_SETTINGS_MOVIE:
                raise ValueError('Patched frontend verification failed')
            _add_system_link(movie)
        if current != ONLINE_MOVIE:
            if hashlib.sha256(movie).hexdigest() != SYSTEM_LINK_MOVIE:
                raise ValueError('Patched frontend verification failed')
            _add_online(movie)
        if hashlib.sha256(movie).hexdigest() != ONLINE_MOVIE:
            raise ValueError('Patched frontend verification failed')
        _add_quit(movie)
        if hashlib.sha256(movie).hexdigest() != PATCHED_MOVIE:
            raise ValueError('Patched frontend verification failed')
        _store(archive, entries, entry, offset, movie)
        changed = True

    movie, entry, offset = _read_movie(archive, entries, b'pongpause_001.xeck')
    current = hashlib.sha256(movie).hexdigest()
    if current != PATCHED_PAUSE_MOVIE:
        if current != ORIGINAL_PAUSE_MOVIE:
            raise ValueError('Unsupported pause menu; no changes made')
        _add_pause_pc_settings(movie)
        if hashlib.sha256(movie).hexdigest() != PATCHED_PAUSE_MOVIE:
            raise ValueError('Patched pause menu verification failed')
        _store(archive, entries, entry, offset, movie)
        changed = True

    if not changed:
        print('Menu patches already verified.', flush=True)
        return
    temp = path.with_suffix('.tmp')
    temp.write_bytes(archive)
    temp.replace(path)
    print('Created and verified offline menu, intro timing, PC settings, system link, online and quit patches locally.', flush=True)


def _patch_offline(movie):
    # AVM1 Jump skips the entire removed expression, including NewObject.
    for start, end in ((0x17F0D, 0x17F35), (0x3AC212, 0x3AC23D)):
        movie[start:end] = b'\x99\x02\x00' + struct.pack('<h', end - start - 5) + bytes(end - start - 5)
    for address, value in ((0x17FE5, 5), (0x3AC24D, 3), (0x3AC278, 4)):
        movie[address] = value
    # Zero only the fixed startup frame thresholds; retain initialization gates.
    for address in (0x3AABB7, 0x3AB044, 0x3AAC74):
        movie[address + 4:address + 8] = bytes(4)
    if hashlib.sha256(movie).hexdigest() != OFFLINE_MOVIE:
        raise ValueError('Patched frontend verification failed')


def _store(archive, entries, entry, offset, movie):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    replacement = compressor.compress(movie) + compressor.flush()
    limit = min((e[2] for e in entries if e[2] > offset), default=len(archive))
    if offset + len(replacement) > limit:
        raise ValueError('Patched movie exceeds archive slot')
    archive[offset:limit] = replacement + bytes(limit - offset - len(replacement))
    struct.pack_into('<I', archive, entry + 8, len(replacement))
