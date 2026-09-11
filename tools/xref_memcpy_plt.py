#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 svc.bin 找到所有对 memcpy/__memcpy_chk/memmove 的 BL 调用点（线性扫描）。"""
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM

BIN = 'native/mtkvapi_work/mtktvapi_service.bin'

def main():
    with open(BIN, 'rb') as f:
        e = ELFFile(f)
        text = None; plt = None
        relplt = None; dyn = None
        for s in e.iter_sections():
            if s.name == '.text':
                text = (s['sh_addr'], s.data())
            elif s.name == '.plt':
                plt = (s['sh_addr'], s.data())
            elif s.name == '.rel.plt':
                relplt = s
            elif s.name == '.dynsym':
                dyn = s
        text_va, text_data = text
        plt_va, _plt_data = plt

        entries = []
        for i, rel in enumerate(relplt.iter_relocations()):
            symidx = rel['r_info_sym']
            name = dyn.get_symbol(symidx).name if symidx < dyn.num_symbols() else '?'
            entries.append((i, name, rel['r_offset']))

    targets = {'memcpy', '__memcpy_chk', 'memmove'}
    stub_map = {}
    plto = 16
    for (idx, name, goo) in entries:
        if name in targets:
            stub = plt_va + plto + idx * 12
            stub_map[stub] = name
    print("[plt] 目标 stubs:", {hex(k): v for k, v in stub_map.items()})
    if not stub_map:
        print("未找到 memcpy 相关 PLT"); return

    def scan(data, base, mode):
        md = Cs(CS_ARCH_ARM, mode); md.detail = True
        res = []
        for ins in md.disasm(data, base):
            if ins.mnemonic in ('bl', 'blx', 'b', 'bx'):
                for op in ins.operands:
                    if op.type == 2 and op.imm:
                        if (op.imm & ~1) in stub_map:
                            res.append((ins.address, ins.mnemonic, stub_map[op.imm & ~1]))
                            break
        return res

    hits_th = scan(text_data, text_va, CS_MODE_THUMB)
    print(f"\n=== memcpy/memmove 调用点 (Thumb) {len(hits_th)} ===")
    for a, m, n in hits_th:
        print(f"  0x{a:08x}  {m}  {n}")

if __name__ == '__main__':
    main()