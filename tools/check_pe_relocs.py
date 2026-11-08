#!/usr/bin/env python3
# Windows の 64bit の DLL・exe に、MinGW の 32 ビットの pseudo relocation が残っていないかを見る。
#
# 取り込んだ関数やデータを __imp_ を通さずに直に指すと、ld は auto-import で「読み込んだあとに
# ランタイムが番地を書き換える」表（pseudo relocation）を作る。32 ビット幅の欄だと、相手の DLL が
# 2GB より遠くに置かれた時点で "32 bit pseudo relocation ... out of range" と言って落ちる。
# 64bit の Windows ではプラグインと msvcrt.dll はほぼ必ずそれだけ離れるので、どのホストでも
# 読み込めない（2026-10 のリリース。#241・#246。crt を r495 に戻した組み合わせで _fpreset がこうなった）。
#
#   python3 tools/check_pe_relocs.py build/S-MU2000.dll build/S-MU2000.clap ...
#
# 32bit（PE32）の物は番地が 4GB に収まるので見ない。見つかったら 1 を返す。
import struct
import sys


def pseudo_relocs(path):
    b = open(path, "rb").read()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    if b[pe:pe + 4] != b"PE\0\0":
        raise ValueError("PE ではない")
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    optsz = struct.unpack_from("<H", b, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", b, opt)[0] != 0x20B:
        return None                          # PE32
    imp_rva = struct.unpack_from("<I", b, opt + 112 + 8)[0]
    secs = []
    for i in range(nsec):
        s = opt + optsz + i * 40
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", b, s + 8)
        secs.append((va, max(vsz, rsz), rptr))

    def off(rva):
        for va, size, rptr in secs:
            if va <= rva < va + size:
                return rptr + rva - va
        raise ValueError("RVA 0x%x がどの区画にも無い" % rva)

    # 取り込み表の欄（IAT の番地）→ 「DLL:名前」
    iat = {}
    d = off(imp_rva) if imp_rva else None
    while d is not None:
        ilt, _, _, name_rva, iat_rva = struct.unpack_from("<IIIII", b, d)
        if name_rva == 0:
            break
        dll = b[off(name_rva):].split(b"\0")[0].decode(errors="replace")
        t = off(ilt or iat_rva)
        k = 0
        while True:
            e = struct.unpack_from("<Q", b, t + k * 8)[0]
            if e == 0:
                break
            if e >> 63:
                name = "#%d" % (e & 0xFFFF)
            else:
                name = b[off(e & 0x7FFFFFFF) + 2:].split(b"\0")[0].decode(errors="replace")
            iat[iat_rva + k * 8] = dll + ":" + name
            k += 1
        d += 20

    # 版 2 の表は 0, 0, 1 の 3 語で始まり、その後に（取り込み欄, 書き換える場所, ビット幅）が並ぶ
    found = []
    magic = struct.pack("<III", 0, 0, 1)
    pos = b.find(magic)
    while pos >= 0:
        q = pos + 12
        while q + 12 <= len(b):
            sym, target, bits = struct.unpack_from("<III", b, q)
            if sym not in iat or bits not in (8, 16, 32, 64):
                break
            found.append((bits, target, iat[sym]))
            q += 12
        pos = b.find(magic, pos + 4)
    return found


def main(paths):
    bad = 0
    for path in paths:
        relocs = pseudo_relocs(path)
        if relocs is None:
            print("%s: 32bit なので見ない" % path)
            continue
        narrow = [r for r in relocs if r[0] < 64]
        for bits, target, sym in narrow:
            print("NG: %s: %d ビットの pseudo relocation（RVA 0x%08x）→ %s" % (path, bits, target, sym))
        if narrow:
            bad += 1
        else:
            print("OK: %s（64 ビット幅のもの %d 個）" % (path, len(relocs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")   # Windows の Python は既定が cp932 で、msys の端末では化ける
    if len(sys.argv) < 2:
        print("使い方: check_pe_relocs.py <DLL か exe> ...", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
