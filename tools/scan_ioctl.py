#!/usr/bin/env python3
# 扫描 ELF 中所有 magic='M'(0x4d) 的 _IOC 命令常量 (32-bit LE)
import sys, struct, os

def scan(path, align=None):
    data = open(path, 'rb').read()
    hits = {}
    step = align if align else 1
    for off in range(0, len(data) - 4, step):
        v = struct.unpack_from('<I', data, off)[0]
        if (v & 0xFF00) != 0x4D00:
            continue
        nr   = v & 0xFF
        size = (v >> 16) & 0x3FFF
        dirn = (v >> 30) & 0x3
        if dirn == 0:
            continue
        if size == 0 or size > 0x2000:
            continue
        hits.setdefault(v, 0)
        hits[v] += 1
    return hits

DN = {0:'NONE', 1:'W', 2:'R', 3:'RW'}
name = {0xC0304D03:'mma_alloc_internal',0x40284D06:'mma_map',0xC0304D0B:'mma_get_meminfo',
        0xC0304D0C:'mma_get_heapinfo',0x80304D02:'mma_get_pipeid',0xC01C4D0D:'mma_query_buf_tag',
        0xC0304D17:'mma_va2iova',0x40284D08:'mma_flush',0x40304D04:'mma_buffer_authorize',
        0x40284D05:'mma_free',0xC0284D09:'mma_export_globalname',0xC0284D0A:'mma_import_globalname',
        0xC0A84D00:'mma_reserve_iova',0x40184D0A:'MsOS_MPool_SetWatchPT'}

allhits = {}
for f in sys.argv[1:]:
    h = scan(f)
    print(f"### {os.path.basename(f)}: {len(h)} 个候选")
    for v, c in sorted(h.items()):
        allhits.setdefault(v, []).append(os.path.basename(f))

print("\n### 合并去重后的 magic='M' 命令表")
print(f"{'cmd':>12} {'dir':>4} {'nr':>4} {'size':>6}  count  name")
for v in sorted(allhits):
    nr   = v & 0xFF
    size = (v >> 16) & 0x3FFF
    dirn = (v >> 30) & 0x3
    nm = name.get(v, '')
    print(f"{v:#012x} {DN[dirn]:>4} {nr:#5x} {size:6d}  {len(allhits[v]):5d}  {nm}")
print(f"\n合计 {len(allhits)} 条")
