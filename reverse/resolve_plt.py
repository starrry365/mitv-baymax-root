#!/usr/bin/env python3
"""把 ARM 32-bit ELF 的 PLT 桩地址解析成导入符号名。
用法: python resolve_plt.py <elf> <stub_addr_hex> [...]
ARM PLT 桩:
    add r12, pc, #A        <- ARM 旋转立即数
    add r12, r12, #B       <- ARM 旋转立即数
    ldr pc, [r12, #C]!     <- 12-bit 直接偏移
GOT 槽 = (桩地址 + 8) + A + B + C
"""
import sys, struct, subprocess, re, os

def ror(x, n):
    n &= 31
    return ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF

def arm_imm12(ins):
    """解码 ARM 数据处理指令的 12-bit 旋转立即数"""
    imm12 = ins & 0xFFF
    rotate = (imm12 >> 8) & 0xF
    imm8 = imm12 & 0xFF
    return ror(imm8, rotate * 2)

def load_relplt(elf, readelf):
    out = subprocess.run([readelf, '-r', elf], capture_output=True, text=True,
                         errors='ignore').stdout
    m = {}
    inplt = False
    for line in out.splitlines():
        if '.rel.plt' in line:
            inplt = True; continue
        if inplt and line.startswith('Relocation section'):
            break
        if not inplt:
            continue
        f = line.split()
        if len(f) >= 5 and re.fullmatch(r'[0-9a-f]{8,16}', f[0]):
            off = int(f[0], 16)
            sym = f[-1]
            if sym and not re.fullmatch(r'[0-9a-f]+', sym):
                m[off] = sym
    return m

def main():
    elf = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    readelf = os.path.join(here, 'llvm-readelf.exe')
    if not os.path.exists(readelf):
        readelf = 'llvm-readelf'
    rel = load_relplt(elf, readelf)
    print(f"# .rel.plt 条目 {len(rel)} 个")
    data = open(elf, 'rb').read()
    for a in sys.argv[2:]:
        va = int(a, 16)
        i0, i1, i2 = struct.unpack_from('<III', data, va)
        A = arm_imm12(i0); B = arm_imm12(i1); C = i2 & 0xFFF
        got = (va + 8) + A + B + C
        print(f"{va:#010x} -> GOT {got:#010x} -> {rel.get(got, '???')}")

main()
