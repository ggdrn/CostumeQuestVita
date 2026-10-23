#!/usr/bin/env python3
"""Execute the patched AVM1 against real game data, without running the game.
Usage: python3 tests/notebook_gfx_test.py [prepared asset directory]
Requires a host C compiler and zlib; contains no game artwork or original scripts.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
ASSET = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/costumequest/asset'
u16 = lambda b, i: struct.unpack_from('<H', b, i)[0]


def constant_pool(raw):
    # Notebook.Quests init action. The production patch validates the full CRC.
    p = 70520
    assert raw[p] == 0x88
    data = raw[p + 3:p + 3 + u16(raw, p + 1)]
    return [s.decode() for s in data[2:].split(b'\0')[:u16(data, 0)]]


def getter_bytecode(pool):
    def push(name):
        return bytes((0x96, 4, 0, 4, 1, 8, pool.index(name)))
    # return this.dragging ? this.target_y : this._y;
    return (push('dragging') + b'\x4e\x12\x9d\x02\0\x09\0' +
            push('target_y') + b'\x4e\x3e' + push('_y') + b'\x4e\x3e')


def holder_pool(raw):
    p = 69642
    assert raw[p] == 0x88
    data = raw[p + 3:p + 3 + u16(raw, p + 1)]
    return [s.decode() for s in data[2:].split(b'\0')[:u16(data, 0)]]


def execute(code, pool, obj, globals_):
    """Small AVM1 interpreter for the opcodes used by the actual update/input.
    Calls the original handleInput bytecode, rather than a Python translation.
    """
    stack, regs, pc = [], {1: obj, 2: globals_}, 0
    while pc < len(code):
        op = code[pc]
        pc += 1
        if op == 0:
            return
        data = b''
        if op >= 128:
            size = u16(code, pc)
            pc += 2
            data = code[pc:pc + size]
            pc += size
        if op == 0x96:
            i = 0
            while i < len(data):
                kind = data[i]
                i += 1
                if kind in (4, 8):
                    value = regs.get(data[i]) if kind == 4 else pool[data[i]]
                    i += 1
                elif kind == 9:
                    value = pool[u16(data, i)]
                    i += 2
                elif kind == 7:
                    value = struct.unpack_from('<i', data, i)[0]
                    i += 4
                elif kind == 6:
                    value = struct.unpack('<d', data[i + 4:i + 8] + data[i:i + 4])[0]
                    i += 8
                elif kind == 0:
                    end = data.index(0, i)
                    value = data[i:end].decode()
                    i = end + 1
                elif kind == 5:
                    value = bool(data[i])
                    i += 1
                else:
                    raise AssertionError(('push type', kind))
                stack.append(value)
        elif op == 0x87:
            regs[data[0]] = stack[-1]
        elif op == 0x3e:
            return stack.pop()
        elif op == 0x17:
            stack.pop()
        elif op == 0x1c:
            stack.append(globals_[stack.pop()])
        elif op == 0x4e:
            name, target = stack.pop(), stack.pop()
            stack.append(target[name])
        elif op == 0x4f:
            value, name, target = stack.pop(), stack.pop(), stack.pop()
            target[name] = value
        elif op == 0x52:
            method, target, count = stack.pop(), stack.pop(), stack.pop()
            args = [stack.pop() for _ in range(int(count))]
            stack.append(target[method](*args))
        elif op == 0x12:
            stack.append(not stack.pop())
        elif op == 0x4c:
            stack.append(stack[-1])
        elif op in (0x99, 0x9d):
            if op == 0x99 or stack.pop():
                pc += struct.unpack('<h', data)[0]
        elif op in (0x0b, 0x0c, 0x0d, 0x47, 0x48, 0x49, 0x67):
            right, left = stack.pop(), stack.pop()
            if op == 0x0b: value = left - right
            elif op == 0x0c: value = left * right
            elif op == 0x0d: value = left / right
            elif op == 0x47: value = left + right
            elif op == 0x48: value = left < right
            elif op == 0x49: value = left == right
            else: value = left > right
            stack.append(value)
        else:
            raise AssertionError(('opcode', hex(op), pc - 1))


def scene(raw, pool):
    nav = {'stick_y': 0, 'stick_rt_y': 0}
    env = {'Math': {'max': max, 'min': min, 'abs': abs}, 'g_Navigator': nav,
           'RunGameCommand': lambda command: True}
    obj = {'pageHeight': 400}
    for holder, bar, extent in [('activeQuestHolder', 'scrollbar_left_mc', 600),
                                ('completedQuestHolder', 'scrollbar_right_mc', 900)]:
        page = {'_y': 0, 'max_y': 0, 'min_y': -extent, '_height': 400 + extent,
                'target_y': 0, 'dragging': False}
        getter_size = u16(raw, 0x112bf)
        page['getTargetY'] = lambda page=page: execute(raw[0x112c1:0x112c1 + getter_size], holder_pool(raw), page, env)
        scrollbar = {'bar_percent': 0}
        scrollbar['checkpos'] = lambda scrollbar=scrollbar, extent=extent: scrollbar['bar_percent'] * extent
        scrollbar['SetPosition'] = lambda percent, scrollbar=scrollbar: scrollbar.update(bar_percent=max(0, min(1, percent)))
        scrollbar['ForcePosition'] = lambda pixels, scrollbar=scrollbar, extent=extent: scrollbar['SetPosition'](pixels / extent)
        obj[holder], obj[bar] = page, scrollbar
    def scroll_bars():
        for holder, bar in [('activeQuestHolder', 'scrollbar_left_mc'), ('completedQuestHolder', 'scrollbar_right_mc')]:
            obj[holder]['_y'] = obj[holder]['max_y'] - obj[bar]['checkpos']()
    obj['scrollBars'] = scroll_bars
    obj['handleInput'] = lambda: execute(raw[0x12580:0x126c5], pool, obj, env)
    return obj, nav, env


def main():
    with tempfile.TemporaryDirectory(prefix='cq-gfx-test-') as tmp:
        library = Path(tmp) / 'controls_gfx.dylib'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Isource', '-dynamiclib',
                        'source/controls_gfx.c', '-lz', '-o', str(library)], cwd=ROOT, check=True)
        patch = ctypes.CDLL(str(library)).controls_gfx_patch
        patch.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        patch.restype = ctypes.c_size_t
        movies = []
        for kind, name in enumerate(['HUDInteractions', 'HUDInteractions_DLC', 'Notebook_DLC']):
            src = (ASSET / f'Data/UI/{name}/Opt/{name}.gfx').read_bytes()
            out = ctypes.create_string_buffer(len(src))
            assert patch(kind, src, len(src), out, len(src)) == len(src)
            raw = src[:8] + zlib.decompress(src[8:])
            patched = out.raw[:8] + zlib.decompress(out.raw[8:])
            assert len(patched) == struct.unpack_from('<I', patched, 4)[0]
            assert not patch(kind, src, len(src), out, len(src) - 1)
            corrupt = bytearray(src)
            corrupt[12] ^= 0x40
            assert not patch(kind, bytes(corrupt), len(src), out, len(src))
            if kind < 2:
                offset = [0x26e4, 0x263b][kind]
                assert raw[offset:offset + 7] == bytes.fromhex('d540000a000000')
                assert patched[offset:offset + 7] == bytes.fromhex('d9400005000000')
            movies.append((raw, patched))
        raw, patched = movies[2]
        pool = constant_pool(raw)
        new_getter = getter_bytecode(holder_pool(raw))
        assert len(new_getter) == 32
        assert patched[0x112c1:0x112c1 + len(new_getter)] == new_getter
        assert u16(patched, 0x112bf) == 32
        # Quests.update must stay byte-for-byte native, including its function
        # header/register bindings; the previous synthetic replacement failed
        # in Scaleform despite passing the isolated VM model.
        assert patched[0x127b7:0x1291d] == raw[0x127b7:0x1291d]
        init_body = 0x112c1 + 32 + 19
        assert u16(patched, init_body - 2) == 27
        page = {'_y': 70}
        execute(patched[init_body:init_body + 27], holder_pool(raw), page, {})
        assert page == {'_y': 70, 'orig_y': 70, 'target_y': 70}
        new_code = patched[0x127c2:0x1291d]
        old_code = raw[0x127c2:0x1291d]
        # Reproduce the bug with original bytecode, then execute the fix.
        old, nav, env = scene(raw, pool)
        nav['stick_rt_y'] = -1
        for _ in range(20): execute(old_code, pool, old, env)
        assert old['completedQuestHolder']['_y'] == 0
        fixed, nav, env = scene(patched, pool)
        nav.update(stick_y=-1, stick_rt_y=-1)
        execute(new_code, pool, fixed, env)
        for _ in range(20): execute(new_code, pool, fixed, env)
        assert round(fixed['activeQuestHolder']['_y']) == -200
        assert round(fixed['completedQuestHolder']['_y']) == -200
        nav.update(stick_y=0, stick_rt_y=0)
        execute(new_code, pool, fixed, env)
        assert round(fixed['completedQuestHolder']['_y']) == -210
        execute(new_code, pool, fixed, env)
        assert round(fixed['completedQuestHolder']['_y']) == -210
        # Scrollbar taps/drags persist when no page drag is active.
        fixed['scrollbar_right_mc']['SetPosition'](0.5)
        execute(new_code, pool, fixed, env)
        assert fixed['completedQuestHolder']['_y'] == -450
        # Page swipes synchronize their scrollbar, clamp, and persist on release.
        page = fixed['completedQuestHolder']
        page.update(dragging=True, target_y=-300)
        execute(new_code, pool, fixed, env)
        assert page['_y'] == -300
        page.update(dragging=False)
        execute(new_code, pool, fixed, env)
        assert page['_y'] == -300
        page.update(dragging=True, target_y=-5000)
        execute(new_code, pool, fixed, env)
        assert page['_y'] == page['min_y']
        page.update(target_y=5000)
        execute(new_code, pool, fixed, env)
        assert page['_y'] == page['max_y']
        print('GFX overrides verified; original scroll bug reproduced; analogs, scrollbar, page drag and limits passed')


if __name__ == '__main__':
    main()
