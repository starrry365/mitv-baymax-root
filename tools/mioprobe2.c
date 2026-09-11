/*
 * mioprobe2.c —— 分段、最小暴露面的物理内存只读探测
 *
 * 教训: 上次 mio_probe 一次性扫了 19 个候选基址 + 数万个 64KB 步长的大范围扫描,
 *       结果把设备扫挂了。本版原则:
 *       1) 每次只碰【十几个】物理页
 *       2) 优先读 cmdline 中明确存在的地址 (recovery_fbaddr / VOC_MEM / CMA0 等 => 一定是真实 DRAM)
 *       3) Image magic 只在极低地址做点状抽查, 不做步进扫描
 *       4) 全程 PROT_READ, 逐条 flush 日志
 *
 * 用法: mioprobe2 <stage>
 *   stage 1: 校准 + 已知 DRAM 页 + 低地址点抽查
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
#include <sys/mman.h>

#define LOGPATH "/data/data/com.mediatek.tv.factory/mioprobe2.log"
#define IMAGE_MAGIC 0x644d5241u

static FILE *g_log;
static void logf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
}

static int g_ps;
static void *map_phys(int fd, uint64_t phys, size_t len) {
    uint64_t pg = phys & ~(uint64_t)(g_ps - 1);
    size_t off  = (size_t)(phys - pg);
    void *b = mmap(NULL, off + len, PROT_READ, MAP_SHARED, fd, (off_t)pg);
    if (b == MAP_FAILED) return MAP_FAILED;
    return (void *)((uintptr_t)b + off);
}
static void unmap(void *p, size_t len) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(g_ps - 1);
    munmap((void *)a, (size_t)((uintptr_t)p - a) + len);
}
static void hexdump(const unsigned char *p, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        logf("        %04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < len; j++) logf("%02x ", p[i+j]);
        logf("  |");
        for (size_t j = 0; j < 16 && i + j < len; j++)
            logf("%c", (p[i+j] >= 32 && p[i+j] < 127) ? p[i+j] : '.');
        logf("|\n");
    }
}

int main(int argc, char **argv) {
    int stage = (argc >= 2) ? atoi(argv[1]) : 1;
    g_ps = sysconf(_SC_PAGESIZE);
    g_log = fopen(LOGPATH, "w");
    if (g_log) setvbuf(g_log, NULL, _IONBF, 0);
    logf("[*] mioprobe2 stage=%d uid=%d\n", stage, getuid());

    int fd = open("/dev/miomap", O_RDWR);
    if (fd < 0) { logf("[!] open O_RDWR: %s, try O_RDONLY\n", strerror(errno));
                  fd = open("/dev/miomap", O_RDONLY); }
    if (fd < 0) { logf("[!] open failed: %s (%d)\n", strerror(errno), errno); return 1; }
    logf("[+] /dev/miomap opened fd=%d\n", fd);

    /* ---- A. cmdline 中确认存在的 DRAM 页 ---- */
    const struct { uint64_t pa; const char *desc; } known[] = {
        { 0x27800000ULL, "recovery_fbaddr" },
        { 0x29600000ULL, "VOC_MEM" },
        { 0x2A400000ULL, "CMA0 start" },
    };
    for (unsigned i = 0; i < sizeof(known)/sizeof(known[0]); i++) {
        unsigned char *p = (unsigned char *)map_phys(fd, known[i].pa, 64);
        if (p == MAP_FAILED) {
            logf("    [0x%09llx] %-16s mmap失败 %s (%d)\n",
                 (unsigned long long)known[i].pa, known[i].desc, strerror(errno), errno);
            continue;
        }
        logf("    [0x%09llx] %-16s OK\n", (unsigned long long)known[i].pa, known[i].desc);
        hexdump(p, 64);
        unmap(p, 64);
    }

    /* ---- B. 低地址点抽查 ARM64 Image magic ---- */
    const uint64_t spots[] = {
        0x00000000ULL, 0x00010000ULL, 0x00080000ULL, 0x00100000ULL,
        0x00200000ULL, 0x00800000ULL, 0x01000000ULL, 0x02000000ULL,
    };
    for (unsigned i = 0; i < sizeof(spots)/sizeof(spots[0]); i++) {
        unsigned char *p = (unsigned char *)map_phys(fd, spots[i], 0x80);
        if (p == MAP_FAILED) {
            logf("    [0x%09llx] mmap失败 %s (%d)\n",
                 (unsigned long long)spots[i], strerror(errno), errno);
            continue;
        }
        uint32_t m = 0; memcpy(&m, p + 0x38, 4);
        uint64_t to = 0, isz = 0;
        memcpy(&to,  p + 0x08, 8);
        memcpy(&isz, p + 0x10, 8);
        logf("    [0x%09llx] magic=0x%08x text_offset=0x%llx image_size=0x%llx  head: %02x %02x %02x %02x %02x %02x %02x %02x\n",
             (unsigned long long)spots[i], m, (unsigned long long)to, (unsigned long long)isz,
             p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
        if (m == IMAGE_MAGIC) {
            logf("        >>> ARM64 IMAGE HEADER FOUND <<<\n");
            hexdump(p, 0x60);
        }
        unmap(p, 0x80);
    }

    close(fd);
    logf("[*] mioprobe2 stage %d 完成\n", stage);
    return 0;
}
