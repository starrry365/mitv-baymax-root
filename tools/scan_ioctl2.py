#!/usr/bin/env python3
# 对齐扫描 + 跨文件交叉验证 magic='M'(0x4d) 的 _IOC 命令常量
import sys, struct, os

KNOWN = {0xC0304D03,0x40284D06,0xC0304D0B,0xC0304D0C,0x80304D02,0xC01C4D0D,0xC0304D17,
         0x40284D08,0x40304D04,0x40284D05,0xC0284D09,0xC0284D0A,0xC0A84D00,0x80044D00,
         0x40184D0A,0x80954D1F}
NAME = {0xC0304D03:'mma_alloc_internal',0x40284D06:'mma_map',0xC0304D0B:'mma_get_meminfo',
        0xC0304D0C:'mma_get_heapinfo',0x80304D02:'mma_get_pipeid',0xC01C4D0D:'mma_query_buf_tag',
        0xC0304D17:'mma_va2iova',0x40284D08:'mma_flush',0x40304D04:'mma_buffer_authorize',
        0x40284D05:'mma_free',0xC0284D09:'mma_export_globalname',0xC0284D0A:'mma_import_globalname',
        0xC0A84D00:'mma_reserve_iova',0x80044D00:'MMA_?(IOR 4B)',0x40184D0A:'MsOS_MPool_SetWatchPT',
        0x80954D1F:'_MHal_XC_GetFlowControlPixelRate'}
DN = {1:'W', 2:'R', 3:'RW'}

def scan(p):
    d = open(p, 'rb').read()
    h = {}
    for off in range(0, len(d) - 4, 4):
        v = struct.unpack_from('<I', d, off)[0]
        if (v & 0xFF00) != 0x4D00:
            continue
        if (v >> 30) & 3 == 0:
            continue
        s = (v >> 16) & 0x3FFF
        if s == 0 or s > 0x1000:
            continue
        h[v] = h.get(v, 0) + 1
    return h

res = {}
for f in sys.argv[1:]:
    h = scan(f)
    print(f"### {os.path.basename(f)} aligned: {len(h)} 条")
    for v, c in h.items():
        res.setdefault(v, {})[os.path.basename(f)] = c

print("\n### 标定: 已知 16 条在对齐扫描中的命中情况")
for v in sorted(KNOWN):
    nm = NAME.get(v, '')
    print(f"  {v:#010x} {DN[(v>>30)&3]:>2} nr={v&0xFF:#04x} size={(v>>16)&0x3FFF:4d} -> {res.get(v,'未命中')}  {nm}")

print("\n### 交叉验证: 至少在 2 个文件中出现的命令")
n = 0
for v, files in sorted(res.items()):
    if len(files) >= 2:
        n += 1
        print(f"  {v:#010x} {DN[(v>>30)&3]:>2} nr={v&0xFF:#04x} size={(v>>16)&0x3FFF:4d}  files={list(files)}  {NAME.get(v,'')}")
print(f"合计 {n} 条跨文件命中")
