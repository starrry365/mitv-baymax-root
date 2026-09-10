#!/usr/bin/env python3
"""从 Thumb-2 反汇编里提取 ioctl 命令常量并解码 _IOC。
用法: python decode_ioctl.py <asm_file> [<asm_file> ...]
支持 ARM 的 movw/movt 双指令构造
"""
import sys, re

DIRN = {0: 'NONE', 1: 'W', 2: 'R', 3: 'RW'}
TYPE = {0x4d: "'M'"}

def decode(cmd):
    nr = cmd & 0xFF
    t = (cmd >> 8) & 0xFF
    size = (cmd >> 16) & 0x3FFF
    d = (cmd >> 30) & 3
    return f"{DIRN[d]:>4} type={TYPE.get(t, hex(t))} nr={nr:#04x} size={size}"

def main():
    hits = []
    for path in sys.argv[1:]:
        lines = open(path, encoding='utf-8', errors='ignore').read().splitlines()
        cur_func = '?'
        pending = {}     # reg -> (低16位, 行号)
        for i, ln in enumerate(lines):
            m = re.match(r'^\s*([0-9a-f]+) <(.+)>:', ln)
            if m:
                cur_func = m.group(2)
            mw = re.search(r'movw\s+(r\d+),\s*#(0x[0-9a-f]+)', ln)
            mt = re.search(r'movt\s+(r\d+),\s*#(0x[0-9a-f]+)', ln)
            if mw:
                pending[mw.group(1)] = (int(mw.group(2), 16), ln.strip()[:40], cur_func, i)
            if mt:
                r = mt.group(1)
                if r in pending:
                    lo, mwline, fn, li = pending.pop(r)
                    cmd = (int(mt.group(2), 16) << 16) | lo
                    hits.append((fn, cmd, mwline, mt.group(2), li))
    print(f"{'函数':<34} {'ioctl 命令':>12}  解码")
    seen = set()
    for fn, cmd, mw, hi, li in hits:
        key = (fn, cmd)
        if key in seen:
            continue
        seen.add(key)
        print(f"{fn:<34} {cmd:#012x}  {decode(cmd)}")

main()
