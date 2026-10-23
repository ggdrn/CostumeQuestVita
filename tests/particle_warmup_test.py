#!/usr/bin/env python3
"""Validate the warmup fix against both routines in the user's ARM library."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
binary = (ROOT / 'build/costumequest/libCq.so').read_bytes()
u32 = lambda b, off: struct.unpack_from('<I', b, off)[0]
f32 = lambda x: struct.unpack('<f', struct.pack('<f', x))[0]


def warmup_duration(code, requested):
    # Execute the VLDR literal, VCMP/VMRS and conditional VMOV from the binary.
    load = u32(code, 0x10)
    assert load == 0xed9f0a7b
    value = struct.unpack_from('<f', code, 0x10 + 8 + (load & 255) * 4)[0]
    assert u32(code, 0x24) == 0xeeb41ac0
    assert u32(code, 0x28) == 0xeef1fa10
    move = u32(code, 0x2c)
    assert move & 0x0fffffff == 0x0eb00a41
    cond = move >> 28
    assert cond in (4, 12)
    if (requested < value if cond == 4 else requested > value):
        value = requested
    return value


def steps(duration):
    count = 0
    while duration > 0:
        duration = max(0, f32(duration - f32(0.26666668)))
        count += 1
        assert count < 10000
    return count


with tempfile.TemporaryDirectory(prefix='cq-particle-test-') as tmp:
    libpath = str(Path(tmp) / 'particle.dylib')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    str(ROOT / 'source/particle_compat.c'), '-o', libpath], check=True)
    patch = ctypes.CDLL(libpath).particle_warmup_prepare
    patch.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t]
    patch.restype = ctypes.c_int
    for address in (0x7acfc4, 0x7ad1e8):
        original = binary[address:address + 0x208]
        buf = (ctypes.c_uint32 * (len(original) // 4)).from_buffer_copy(original)
        assert patch(buf, len(original)) == 1
        fixed = bytes(buf)
        assert [i for i, (a, b) in enumerate(zip(original, fixed)) if a != b] == [0x2f]
        for requested in (-1, 0, 0.1, 1, 3, 30, 90, 120):
            requested = f32(requested)
            assert warmup_duration(original, requested) == max(90, requested)
            assert warmup_duration(fixed, requested) == min(90, requested)
        assert steps(warmup_duration(original, 1)) == 338
        assert steps(warmup_duration(fixed, 1)) == 4
        assert steps(warmup_duration(fixed, 0)) == 0
        # Refuse changed prologue/comparison/literal, short buffers and reapply.
        assert patch(buf, len(original)) == 0
        for offset in (0, 0x10, 0x24, 0x2c, 0x204):
            bad = bytearray(original)
            bad[offset] ^= 1
            buf = (ctypes.c_uint32 * (len(bad) // 4)).from_buffer_copy(bad)
            assert patch(buf, len(bad)) == 0
            assert bytes(buf) == bad
        buf = (ctypes.c_uint32 * (len(original) // 4)).from_buffer_copy(original)
        assert patch(buf, len(original) - 4) == 0
        assert bytes(buf) == original
print('Particle/ribbon warmup: native instructions, bounds and version guards passed')
