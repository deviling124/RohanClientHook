#!/usr/bin/env python3
"""Finds the seven item tooltip attribute builders of the reference client and the code block of their type-175 case.
Prints a C++ table for src/ItemTooltip/CombatStatLines.cpp. Usage: gen_tooltip_variants.py <rohanclient_unpacked.exe>"""
import re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
d = open(sys.argv[1], 'rb').read()
B = 0x400000
md = Cs(CS_ARCH_X86, CS_MODE_32)
def u32(v): return struct.unpack_from('<I', d, v - B)[0]
JOIN_CALL = 0x90c680
out = []
for m in re.finditer(re.escape(b'\x83\xc0\xeb\x3d\x9a\x00\x00\x00'), d):
    site = B + m.start()
    if site in (0x76f346,): continue
    seg = d[m.start(): m.start() + 0x40]
    ja = seg.find(b'\x0f\x87')
    ja_va = site + ja
    default = ja_va + 6 + struct.unpack_from('<i', seg, ja + 2)[0]
    mv = seg.find(b'\x0f\xb6\x80', ja)          # movzx eax, byte [eax+BYTE]
    byte_tab = struct.unpack_from('<I', seg, mv + 3)[0]
    jm = seg.find(b'\xff\x24\x85', mv)          # jmp [eax*4+JT]
    jt = struct.unpack_from('<I', seg, jm + 3)[0]
    k = d[byte_tab - B + (175 - 21)]
    start = u32(jt + k * 4)
    end = None
    for i in md.disasm(d[start - B: start - B + 0x100], start):
        if i.mnemonic == 'call' and i.op_str == hex(JOIN_CALL):
            end = i.address; break
    block = d[start - B: end - B]
    calls = []; pushaf = pushtext = None
    for i in md.disasm(block, start):
        if i.bytes[0] == 0xE8 and i.size == 5: calls.append(i.address - start)
        if i.mnemonic == 'push' and i.size == 5 and i.bytes[1:5] == b'\xaf\x00\x00\x00': pushaf = i.address - start + 1
        if i.mnemonic == 'push' and i.size == 5 and i.bytes[1:5] == b'\x3b\x19\x00\x00': pushtext = i.address - start + 1
    out.append((site, ja_va, default, start, end, calls, pushaf, pushtext, d[ja_va - B: ja_va - B + 6]))
for o in out:
    print(f"// builder with the add at 0x{o[0]:X}")
    print("{0x%X, 0x%X, 0x%X, 0x%X, 0x%X, {%s}, %s, %s}," % (o[1], o[2], o[3], o[4], o[4]-o[3], ", ".join(map(str, o[5])), o[6], o[7]))
