#!/usr/bin/env python3
"""Audit the supplied PowerVR APK against the control bridge and generated icons."""
import ctypes, io, struct, subprocess, sys, tempfile, zipfile, zlib
from pathlib import Path
root=Path(__file__).resolve().parents[1]
apk=Path(sys.argv[1]) if len(sys.argv)>1 else root.parent/'CostumeQuestPowerVR.apk'
def unpack_movie(b):
    return b[:8]+zlib.decompress(b[8:]) if b[:3] in (b'CFX',b'CWS') else b
def tags(b,start=None):
    if start is None: start=8+(5+4*(b[8]>>3)+7)//8+4
    while start+2<=len(b):
        h=struct.unpack_from('<H',b,start)[0];start+=2
        code,n=h>>6,h&63
        if n==63:n=struct.unpack_from('<I',b,start)[0];start+=4
        body=b[start:start+n];assert len(body)==n
        start+=n;yield code,body
        if code==39:yield from tags(body,4)
        if code==0:break
def cstring(b,i):
    end=b.index(0,i);return b[i:end].decode(),end+1
with zipfile.ZipFile(apk) as outer:
    native=outer.read('lib/armeabi-v7a/libCq.so')
    for address,expected in [(0x6e67c4,0xe5c02000),(0x6e691c,0xe5c40000),(0x6e6a20,0x15ca4000),
                             (0x6e21b0,0xe92d47f0),(0x6e21b4,0xe1a04000),
                             (0x87a884,0xe92d4800),(0x87a888,0xe2811f81),
                             (0x899e88,0xe5d9011c),(0x6e332c,0xe3a02fb5)]:
        assert struct.unpack_from('<I',native,address)[0]==expected,hex(address)
    # The motion bridge must use stick units: Android pixels would be multiplied
    # by the console gain, 171x the touch gain. Guard against another APK build.
    touch_gain=struct.unpack_from('<f',native,0x89a36c)[0]
    gamepad_gain=struct.unpack_from('<f',native,0x89a370)[0]
    assert abs(touch_gain-0.0007)<1e-8 and abs(gamepad_gain-0.12)<1e-7
    # _UpdateMinigame loads g_BattleMiniGameTimer through the GOT, then
    # subtracts dt. The bridge must not treat each countdown step as a restart.
    for address,expected in [(0x7cc5d8,0xe59f1318),(0x7cc5e8,0xe7910002),
                             (0x7cc5ec,0xed900a00),(0x7cc61c,0xee301a41)]:
        assert struct.unpack_from('<I',native,address)[0]==expected,hex(address)
    assert struct.unpack_from('<I',native,0xc42738)[0]==0xc66f30
    print('Native minigame countdown verified (timer -= dt)')
    # PrepareMiniGame resolves RandomPress/Mash/Meter (21/22/23) before input.
    for address,expected in [(0x7cf9bc,0xe3500017),(0x7cf9e4,0xe3500015),
                             (0x7cfa08,0xe3500016),(0x899e80,0xe3530014)]:
        assert struct.unpack_from('<I',native,address)[0]==expected,hex(address)
    print(f'Motion gain verified: touch={touch_gain:.4f}, gamepad={gamepad_gain:.2f}; random prompts 21-23 resolved natively')
    assets=zipfile.ZipFile(io.BytesIO(outer.read('assets/ant.mp3')))
    with tempfile.TemporaryDirectory() as td:
        lib=Path(td)/'icons.so'
        subprocess.run(['cc','-shared','-fPIC',str(root/'source/icons_movie.c'),'-lz','-o',str(lib)],check=True)
        dll=ctypes.CDLL(str(lib));dll.icons_movie_build.restype=ctypes.c_size_t
        buffer=ctypes.create_string_buffer(4096)
        size=dll.icons_movie_build(buffer,4096)
        assert size and size<=len(assets.read('Data/UI/Mobile_InputIcons/Opt/Mobile_InputIcons.gfx'))
        built=unpack_movie(buffer.raw[:size]);exports=set();textures=[]
        for code,b in tags(built):
            if code==56:
                n=struct.unpack_from('<H',b)[0];i=2
                for _ in range(n):i+=2;name,i=cstring(b,i);exports.add(name)
            if code==1001:
                i=8;n=b[i];i+=1+n;n=b[i];i+=1;textures.append(b[i:i+n].decode())
        names={n.lower() for n in assets.namelist()}
        for texture in textures:
            folder='ps3_controllericons' if texture.startswith('PS3__') else 'mobile_inputicons'
            stem=texture.removeprefix('PS3__').removesuffix('.dds').lower()
            assert f'ogles/munged/ui/flash/{folder}/{stem}.txtr~c' in names,texture
        screens=[];missing={};count=0
        for path in assets.namelist():
            if not path.endswith('.gfx'):continue
            count+=1;movie=unpack_movie(assets.read(path));imports=set()
            for code,b in tags(movie):
                if code not in (57,71):continue
                url,i=cstring(b,0)
                if code==71:i+=2
                n=struct.unpack_from('<H',b,i)[0];i+=2
                for _ in range(n):
                    i+=2;name,i=cstring(b,i)
                    if 'controllericons' in url.lower():imports.add(name)
            if imports:
                screens.append(path)
                if imports-exports:missing[path]=sorted(imports-exports)
        print(f'APK: {len(assets.namelist())} entries, {count} GFX movies, {len(screens)} movies importing controller icons')
        print(f'Generated library: {len(exports)} exports, {len(textures)} existing textures, {size} bytes')
        print('Missing imported symbols:',missing)
        assert not missing
        print('Native signatures, icon imports and texture references OK')
