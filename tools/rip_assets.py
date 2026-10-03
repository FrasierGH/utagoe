"""Rip the original program's artwork into ``res/``.

Usage: ``python tools/rip_assets.py [path/to/utagoe.exe]`` (default:
``original/utagoe30/utagoe.exe``)

Copies, byte for byte, the button glyphs (from the main form's DFM), the stock
VCL button and media-player bitmaps, the About logo and the application icon
out of the PE resources, as .bmp/.ico files for the resource compiler.  Only
the standard library is used.

Utagoe Rip is freeware by TODAKEN; its readme says redistribution is free
("転載・配付は自由です").
"""

from __future__ import annotations

import os
import struct
import sys
from typing import Dict, Tuple

RT_BITMAP, RT_ICON, RT_RCDATA, RT_GROUP_ICON = 2, 3, 10, 14


def read_resources(path: str) -> Dict[Tuple[object, object], bytes]:
    """``{(type, name): data}`` for the first language of every resource."""
    with open(path, "rb") as fh:
        pe = fh.read()
    peoff = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[peoff: peoff + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec, = struct.unpack_from("<H", pe, peoff + 6)
    optsize, = struct.unpack_from("<H", pe, peoff + 20)
    opt = peoff + 24
    magic, = struct.unpack_from("<H", pe, opt)
    ddir = opt + (96 if magic == 0x10B else 112)
    res_rva, _ = struct.unpack_from("<II", pe, ddir + 2 * 8)
    secs = []
    for i in range(nsec):
        s = opt + optsize + 40 * i
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", pe, s + 8)
        secs.append((va, max(vsize, rawsize), rawptr))

    def off(rva: int) -> int:
        for va, size, raw in secs:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError("bad RVA")

    base = off(res_rva)

    def entries(o: int):
        named, ids = struct.unpack_from("<HH", pe, o + 12)
        for k in range(named + ids):
            name, target = struct.unpack_from("<II", pe, o + 16 + 8 * k)
            if name & 0x80000000:
                p = base + (name & 0x7FFFFFFF)
                ln, = struct.unpack_from("<H", pe, p)
                key = pe[p + 2: p + 2 + 2 * ln].decode("utf-16-le")
            else:
                key = name
            yield key, target

    out: Dict[Tuple[object, object], bytes] = {}
    for rtype, t1 in entries(base):
        if not t1 & 0x80000000:
            continue
        for rname, t2 in entries(base + (t1 & 0x7FFFFFFF)):
            if not t2 & 0x80000000:
                continue
            for _lang, t3 in entries(base + (t2 & 0x7FFFFFFF)):
                rva, size = struct.unpack_from("<II", pe, base + t3)
                o = off(rva)
                out[(rtype, rname)] = pe[o: o + size]
                break
    return out


def dfm_binary_props(data: bytes) -> Dict[str, Dict[str, bytes]]:
    """Binary properties (vaBinary) of each component of a Delphi form stream."""
    if data[:4] != b"TPF0":
        raise ValueError("not a binary form")
    pos = 4
    comps: Dict[str, Dict[str, bytes]] = {}

    def sstr():
        nonlocal pos
        n = data[pos]
        s = data[pos + 1: pos + 1 + n].decode("cp932", "replace")
        pos += 1 + n
        return s

    def skip_value():
        nonlocal pos
        t = data[pos]
        pos += 1
        if t == 1:                      # list
            while data[pos]:
                skip_value()
            pos += 1
        elif t == 2: pos += 1
        elif t == 3: pos += 2
        elif t in (4, 15): pos += 4
        elif t == 5: pos += 10
        elif t in (6, 7): pos += 1 + data[pos]
        elif t in (10, 12, 20):          # binary / long strings
            n, = struct.unpack_from("<i", data, pos)
            val = data[pos + 4: pos + 4 + n]
            pos += 4 + n
            return val
        elif t == 11:                    # set
            while data[pos]:
                pos += 1 + data[pos]
            pos += 1
        elif t == 14:                    # collection
            while data[pos]:
                if data[pos] in (2, 3, 4):
                    skip_value()
                while data[pos]:
                    sstr()
                    skip_value()
                pos += 1
            pos += 1
        elif t in (16, 17, 19): pos += 8
        elif t == 18:
            n, = struct.unpack_from("<i", data, pos)
            pos += 4 + 2 * n
        return None

    def obj():
        nonlocal pos
        if data[pos] & 0xF0 == 0xF0:
            flags = data[pos] & 0x0F
            pos += 1
            if flags & 2:
                skip_value()
        sstr()
        name = sstr()
        props: Dict[str, bytes] = {}
        while data[pos]:
            key = sstr()
            val = skip_value()
            if val is not None:
                props[key] = val
        pos += 1
        while data[pos]:
            obj()
        pos += 1
        comps[name] = props

    obj()
    return comps


def bmp_file(dib: bytes) -> bytes:
    """Prefix a DIB from RT_BITMAP with a BITMAPFILEHEADER."""
    hsize, _w, _h, _planes, bpp = struct.unpack_from("<IiiHH", dib, 0)
    ncolors = struct.unpack_from("<I", dib, 32)[0] if hsize >= 36 else 0
    pal = ncolors or (1 << bpp if bpp <= 8 else 0)
    offset = 14 + hsize + 4 * pal
    return b"BM" + struct.pack("<IHHI", 14 + len(dib), 0, 0, offset) + dib


MAIN_GLYPHS = {"BitBtn1": "open", "StartBtn": "note", "PlayBtn1": "play", "CloseBtn": "quit",
               "AboutBtn": "about", "SetBitBtn": "settings"}
STOCK = {"ok": "BBOK", "cancel": "BBCANCEL", "help": "BBHELP"}
MEDIA = {"mp_play": "MPPLAY", "mp_pause": "MPPAUSE", "mp_stop": "MPSTOP", "mp_prev": "MPPREV"}


def rip(exe: str, out: str):
    res = read_resources(exe)
    forms = {name: data for (t, name), data in res.items() if t == RT_RCDATA}
    bitmaps = {name: data for (t, name), data in res.items() if t == RT_BITMAP}
    files: Dict[str, bytes] = {}

    main = dfm_binary_props(forms["TFORM1"])
    for comp, role in MAIN_GLYPHS.items():
        files[role + ".bmp"] = main[comp]["Glyph.Data"][4:]          # TBitmap stream: size + BMP file
    for role, rname in STOCK.items():
        files[role + ".bmp"] = bmp_file(bitmaps[rname])
    for role, rname in MEDIA.items():
        files[role + ".bmp"] = bmp_file(bitmaps["CL_" + rname])
        files[role + "_disabled.bmp"] = bmp_file(bitmaps["DI_" + rname])
    pic = dfm_binary_props(forms["TABOUTFORM"])["LogoImg"]["Picture.Data"]
    files["logo.bmp"] = pic[1 + pic[0] + 4:]                            # class name + size + BMP file

    grp = res[(RT_GROUP_ICON, "MAINICON")]
    count, = struct.unpack_from("<H", grp, 4)
    entries, blobs, offset = [], [], 6 + 16 * count
    for k in range(count):
        w, h, colors, _r, planes, bpp, _size, ident = struct.unpack_from("<BBBBHHIH", grp, 6 + 14 * k)
        blob = res[(RT_ICON, ident)]
        entries.append(struct.pack("<BBBBHHII", w, h, colors, 0, planes, bpp, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    files["utagoe.ico"] = struct.pack("<HHH", 0, 1, count) + b"".join(entries) + b"".join(blobs)

    os.makedirs(out, exist_ok=True)
    for name, data in sorted(files.items()):
        path = os.path.join(out, name)
        with open(path, "wb") as fh:
            fh.write(data)
        yield path


if __name__ == "__main__":
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "original", "utagoe30", "utagoe.exe")
    for p in rip(exe, os.path.join(root, "res")):
        print(p)
