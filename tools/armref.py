#!/usr/bin/env python3
# ============================================================
# armref.py — ARM32 (Thumb-2) ELF 字符串/地址交叉引用定位
#
# 用途：给定 .so 里的一个字符串（或绝对地址），找出哪些函数引用了它。
# 覆盖三种引用方式：
#   1) 字面量池：直接出现 4 字节小端 vaddr（在 .text/.data）
#   2) movw/movt 立即数对：movw rX,#lo ; movt rX,#hi  (Thumb-2)
#   3) 数据指针：出现在 .data/.data.rel.ro
#
# 用法:
#   python armref.py <elf> str "/dev/mik!sys"
#   python armref.py <elf> addr 0x1d09fe
#   python armref.py <elf> dis 0x323000 0x60     # 反汇编片段(需 llvm-objdump)
# ============================================================
import struct, sys, subprocess, os

class E32:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        b = self.b
        assert b[4] == 1 and b[5] == 1, "expect ELF32 LE"
        e_shoff = int.from_bytes(b[0x20:0x24], 'little')
        e_shentsize = int.from_bytes(b[0x2e:0x30], 'little')
        e_shnum = int.from_bytes(b[0x30:0x32], 'little')
        e_shstrndx = int.from_bytes(b[0x32:0x34], 'little')
        self.sec = []
        for i in range(e_shnum):
            o = e_shoff + i*e_shentsize
            name, typ, flags, addr, off, size, link, info, align, entsz = \
                struct.unpack_from('<IIIIIIIIII', b, o)
            self.sec.append(dict(i=i, name=name, typ=typ, flags=flags, addr=addr,
                                 off=off, size=size, link=link, info=info, entsz=entsz))
        sh = self.sec[e_shstrndx]
        for s in self.sec:
            st = sh['off'] + s['name']; en = b.index(b'\0', st)
            s['nm'] = b[st:en].decode('latin1')
        self.funcs = self._funcs()

    def _funcs(self):
        out = []
        for ds in self.sec:
            if ds['typ'] not in (2, 11):   # SYMTAB / DYNSYM
                continue
            strsec = self.sec[ds['link']]
            for k in range(ds['size'] // 16):
                o = ds['off'] + k*16
                nm_, val, sz, info, other, shndx = struct.unpack_from('<IIIBBH', self.b, o)
                if info & 0xf != 2: continue
                st = strsec['off'] + nm_; en = self.b.index(b'\0', st)
                out.append((val, sz, self.b[st:en].decode('latin1')))
        out.sort()
        return out

    def func_at(self, v):
        lo, hi, best = 0, len(self.funcs)-1, None
        while lo <= hi:
            m = (lo+hi)//2
            if self.funcs[m][0] <= v: best = self.funcs[m]; lo = m+1
            else: hi = m-1
        if best and (best[1] == 0 or v < best[0]+best[1]): return best
        return None

    def sec_of(self, v):
        for s in self.sec:
            if s['addr'] <= v < s['addr']+s['size'] and s['size']:
                return s
        return None

    def find_str(self, needle):
        nb = needle.encode()
        out = []; i = self.b.find(nb)
        while i != -1:
            s = self.sec_of(i)     # vaddr == fileoff on these libs, but be safe
            out.append((i, s['nm'] if s else '?'))
            i = self.b.find(nb, i+1)
        return out

    def refs_word(self, target):
        needle = struct.pack('<I', target)
        hits = []
        for s in self.sec:
            if not (s['flags'] & 0x2) or s['size'] == 0: continue   # ALLOC
            if s['nm'] in ('.dynsym', '.dynstr', '.gnu.hash', '.rel.dyn', '.rel.plt',
                           '.gnu.version', '.gnu.version_r'): continue
            i = self.b.find(needle, s['off'], s['off']+s['size'])
            while i != -1:
                hits.append((i - s['off'] + s['addr'], s['nm']))
                i = self.b.find(needle, i+1, s['off']+s['size'])
        return hits

    def refs_movwmovt(self, target):
        """扫描 Thumb-2 movw/movt 立即数对，还原出目标地址的引用点"""
        b = self.b
        hits = []
        for s in self.sec:
            if s['nm'] not in ('.text', '.plt') or s['size'] == 0: continue
            off, end = s['off'], s['off']+s['size']
            # 先收集所有 movw / movt
            seen = {}
            i = off
            while i + 4 <= end:
                hw1 = int.from_bytes(b[i:i+2], 'little')
                hw2 = int.from_bytes(b[i+2:i+4], 'little')
                if (hw1 & 0xF800) == 0xF000 and (hw1 & 0x0400) == 0 \
                   and (hw1 & 0x0080) == 0:
                    op = (hw1 >> 4) & 0xF
                    if op in (0x4, 0xC):     # movw / movt
                        imm4 = hw1 & 0xF
                        i_bit = (hw1 >> 10) & 1
                        imm3 = (hw2 >> 12) & 0x7
                        rd = (hw2 >> 8) & 0xF
                        imm8 = hw2 & 0xFF
                        imm16 = (imm4 << 12) | (i_bit << 11) | (imm3 << 8) | imm8
                        seen.setdefault(rd, []).append(
                            (i - off + s['addr'], 'movw' if op == 0x4 else 'movt', imm16))
                        i += 4; continue
                i += 2
            for rd, lst in seen.items():
                for k in range(len(lst)-1):
                    a1, o1, v1 = lst[k]
                    a2, o2, v2 = lst[k+1]
                    if o1 == 'movw' and o2 == 'movt' and a2 - a1 <= 32:
                        addr = (v2 << 16) | v1
                        if addr == target:
                            hits.append((a1, s['nm'], a2))
        return hits

def main():
    if len(sys.argv) < 3: print(__doc__); return
    e = E32(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'str':
        for i, sn in e.find_str(sys.argv[3]):
            print(f"[str] fileoff={i:#x} vaddr={i:#x} sec={sn}")
    elif cmd == 'addr':
        t = int(sys.argv[3], 0)
        print(f"[*] target vaddr = {t:#x}")
        print("--- 字面量池 / 数据指针命中 ---")
        for v, sn in e.refs_word(t):
            f = e.func_at(v)
            print(f"    vaddr={v:#x} sec={sn}" + (f"  -> {f[2]} (+{v-f[0]:#x})" if f else ""))
        print("--- movw/movt 命中 ---")
        for a1, sn, a2 in e.refs_movwmovt(t):
            f = e.func_at(a1)
            print(f"    vaddr={a1:#x}..{a2:#x} sec={sn}" + (f"  -> {f[2]} (+{a1-f[0]:#x})" if f else ""))
    elif cmd == 'dis':
        start = int(sys.argv[3], 0); ln = int(sys.argv[4], 0)
        od = os.environ.get('LLVM_OBJDUMP', 'llvm-objdump')
        r = subprocess.run([od, '-d', '--no-show-raw-insn',
                            '--triple=thumbv7-none-linux-gnueabi',
                            f'--start-address={start}', f'--stop-address={start+ln}',
                            sys.argv[1]], capture_output=True, text=True, errors='replace')
        print(r.stdout, r.stderr)
    else:
        print(__doc__)

main()
