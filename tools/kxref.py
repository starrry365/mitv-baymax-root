#!/usr/bin/env python3
# ============================================================
# kxref.py — ELF(64/32) 符号交叉引用工具，专治 ET_REL（.ko 模块）
#
# ET_REL 里符号的 st_value 是"节内偏移"，llvm-nm 显示的地址会误导。
# 本工具：按节解析 + 重定位表，回答两个问题：
#   xref <symbol>   -> 谁引用了它（重定位点所在节/偏移/所属函数）
#   callers <sym>   -> 同上，但只列 .text 里的调用点
#   func <sym>      -> 打印函数所在节/偏移/文件偏移（供反汇编用）
#
# 用法:
#   python kxref.py mik.ko xref MI_DEV_DEBUG_Sys
#   python kxref.py mik.ko func MI_DEV_DEBUG_Sys
#   python kxref.py mik.ko strings 0x2a3b48 256
# ============================================================
import sys, struct

def u(b, o, n): return int.from_bytes(b[o:o+n], 'little')

class ELF:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        b = self.b
        self.is64 = b[4] == 2
        self.le = b[5] == 1
        assert self.le, "big-endian not supported"
        if self.is64:
            e_shoff = u(b, 0x28, 8); e_shentsize = u(b, 0x3a, 2)
            e_shnum = u(b, 0x3c, 2); e_shstrndx = u(b, 0x3e, 2)
            self.symfmt = ('<IBBHQQ', 24); self.rela = 24; self.sym_ent = 24
        else:
            e_shoff = u(b, 0x20, 4); e_shentsize = u(b, 0x2e, 2)
            e_shnum = u(b, 0x30, 2); e_shstrndx = u(b, 0x32, 2)
            self.symfmt = ('<IIIBBH', 16); self.rela = 8; self.sym_ent = 16
        self.sec = []
        for i in range(e_shnum):
            o = e_shoff + i*e_shentsize
            if self.is64:
                name, typ, flags, addr, off, size, link, info, align, entsz = struct.unpack_from('<IIQQQQIIQQ', b, o)
            else:
                name, typ, flags, addr, off, size, link, info, align, entsz = struct.unpack_from('<IIIIIIIIII', b, o)
            self.sec.append(dict(i=i, name=name, typ=typ, flags=flags, addr=addr,
                                 off=off, size=size, link=link, info=info, entsz=entsz))
        shstr = self.sec[e_shstrndx]
        for s in self.sec:
            s['nm'] = self.cstr(shstr['off'] + s['name'])
        self._symcache = {}

    def cstr(self, off):
        e = self.b.index(b'\0', off)
        return self.b[off:e].decode('latin1')

    def syms(self, sec):
        """返回该节（SHT_SYMTAB/DYNSYM）的符号列表"""
        if sec['i'] in self._symcache: return self._symcache[sec['i']]
        out = []
        entsz = sec['entsz'] or self.sym_ent
        n = sec['size'] // entsz
        strsec = self.sec[sec['link']]
        for k in range(n):
            o = sec['off'] + k*entsz
            if self.is64:
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', self.b, o)
            else:
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', self.b, o)
            nm = self.cstr(strsec['off'] + st_name) if st_name else ''
            out.append(dict(idx=k, name=nm, info=st_info, type=st_info & 0xf,
                            bind=st_info >> 4, shndx=st_shndx, value=st_value, size=st_size))
        self._symcache[sec['i']] = out
        return out

    def symtab(self):
        for s in self.sec:
            if s['typ'] == 2: return s       # SHT_SYMTAB
        for s in self.sec:
            if s['typ'] == 11: return s      # SHT_DYNSYM
        return None

    def byname(self, name):
        st = self.symtab()
        for s in self.syms(st):
            if s['name'] == name: return s
        return None

    def text_funcs(self):
        """所有可执行节里的 FUNC 符号 -> {secidx: [(off,size,name)]}"""
        st = self.symtab()
        d = {}
        for s in self.syms(st):
            if s['type'] != 2: continue          # STT_FUNC
            if s['shndx'] == 0 or s['shndx'] >= len(self.sec): continue
            sec = self.sec[s['shndx']]
            if not (sec['flags'] & 0x4): continue  # SHF_EXECINSTR
            d.setdefault(s['shndx'], []).append((s['value'], s['size'], s['name']))
        for k in d: d[k].sort()
        return d

    def find_func_at(self, shndx, off):
        tf = self.text_funcs().get(shndx, [])
        best = None
        for v, sz, nm in tf:
            if v <= off and (sz == 0 or off < v + sz): best = (v, sz, nm)
            elif v > off: break
        return best

    def xref(self, name):
        st = self.symtab()
        syms = self.syms(st)
        target = None
        for s in syms:
            if s['name'] == name: target = s['idx']; break
        if target is None: return None, None
        hits = []
        for s in self.sec:
            if s['typ'] != 4: continue   # SHT_RELA
            entsz = s['entsz'] or self.rela
            n = s['size'] // entsz
            for k in range(n):
                o = s['off'] + k*entsz
                if self.is64:
                    r_off, r_info, r_add = struct.unpack_from('<QQq', self.b, o)
                    symi = r_info >> 32
                else:
                    r_off, r_info = struct.unpack_from('<II', self.b, o)
                    symi = r_info >> 8
                if symi == target:
                    hits.append((s['info'], r_off, r_add))
        return target, hits

    def where(self, shndx, off):
        sec = self.sec[shndx]
        foff = sec['off'] + off
        f = self.find_func_at(shndx, off) if (sec['flags'] & 0x4) else None
        loc = f"{sec['nm']}+{off:#x} (file {foff:#x})" if not f else \
              f"{f[2]}+{off - f[0]:#x}  [{sec['nm']}+{off:#x}, file {foff:#x}]"
        return loc

def main():
    if len(sys.argv) < 3:
        print(__doc__); return
    e = ELF(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'func':
        s = e.byname(sys.argv[3])
        if not s: print("symbol not found"); return
        sec = e.sec[s['shndx']]
        print(f"{s['name']}  sec={sec['nm']} off={s['value']:#x} size={s['size']} "
              f"fileoff={sec['off']+s['value']:#x}  (for llvm-objdump --start-address)")
    elif cmd == 'xref':
        t, hits = e.xref(sys.argv[3])
        if t is None: print("symbol not found"); return
        print(f"[*] {sys.argv[3]}: {len(hits)} 处引用")
        for shndx, off, add in hits:
            print("    " + e.where(shndx, off))
    elif cmd == 'strings':
        off = int(sys.argv[3], 0); n = int(sys.argv[4], 0)
        b = e.b[off:off+n]
        for i in range(0, len(b), 16):
            print(f"{i:04x}  " + ' '.join(f'{x:02x}' for x in b[i:i+16]))
    else:
        print(__doc__)

main()
