/*
 * miostep.c —— /dev/miomap 分级试探。
 *
 * 背景:
 *   - system_app 域对该设备有 chr_file { read write ioctl map open }
 *   - 但实测 "open + mmap" 之后设备立刻失联(疑似 ARM64 内存属性冲突 -> SError)
 *
 * 本程序的核心设计: 【每一步先把结果 fsync 落盘, 再执行下一步危险动作】
 *   => 即使第 N 步把机器打挂, 重启后从日志也能精确定位是哪一步致命。
 *
 * 试探顺序(由安全到激进):
 *   S1 open()
 *   S2 lseek + read            <- 若驱动用文件偏移当物理地址, 这条路最安全、最优
 *   S3 pread(phys)             <- 绕开可能缺失的 llseek
 *   S4 ioctl 探测(只读极少几条)
 *   S5 mmap(offset=0) + 读 16 字节
 *   S6 mmap(phys=0x27800000) + 读 16 字节
 *
 * 日志同时写 /sdcard/miostep.log 与 factory 目录, 双备份。
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
#include <sys/ioctl.h>

#define LOG_SD   "/sdcard/miostep.log"
#define LOG_DAT  "/data/data/com.mediatek.tv.factory/miostep.log"

static FILE *g_a, *g_b;
static void step(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    if (g_a) { fprintf(g_a, "%s\n", line); fflush(g_a); fsync(fileno(g_a)); }
    if (g_b) { fprintf(g_b, "%s\n", line); fflush(g_b); fsync(fileno(g_b)); }
    sync();
}
static void dump16(const unsigned char *p, size_t n) {
    char hex[256] = {0}, asc[64] = {0};
    char *h = hex; char *a = asc;
    for (size_t i = 0; i < n; i++) {
        h += snprintf(h, hex + sizeof(hex) - h, "%02x ", p[i]);
        a += snprintf(a, asc + sizeof(asc) - a, "%c",
                      (p[i] >= 32 && p[i] < 127) ? p[i] : '.');
    }
    step("        %s |%s|", hex, asc);
}

int main(void) {
    int ps = sysconf(_SC_PAGESIZE);
    remove(LOG_SD);
    g_a = fopen(LOG_SD, "w");
    g_b = fopen(LOG_DAT, "w");
    if (g_a) setvbuf(g_a, NULL, _IONBF, 0);
    if (g_b) setvbuf(g_b, NULL, _IONBF, 0);

    step("[S0] miostep start uid=%d pagesize=%d logs(a=%d b=%d)",
         getuid(), ps, g_a != NULL, g_b != NULL);

    /* ---- S1: open ---- */
    int fd = open("/dev/miomap", O_RDWR);
    step("[S1] open O_RDWR -> fd=%d %s", fd, fd < 0 ? strerror(errno) : "");
    if (fd < 0) {
        fd = open("/dev/miomap", O_RDONLY);
        step("[S1b] open O_RDONLY -> fd=%d %s", fd, fd < 0 ? strerror(errno) : "");
        if (fd < 0) { step("[S1] FATAL open failed, abort"); return 1; }
    }

    unsigned char buf[64];

    /* ---- S2: lseek + read ---- */
    off_t off0 = lseek(fd, 0, SEEK_SET);
    step("[S2] lseek(fd,0,SEEK_SET) -> %lld %s",
         (long long)off0, off0 < 0 ? strerror(errno) : "");
    if (off0 >= 0) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, 16);
        step("[S2] read(16) -> %zd %s", n, n < 0 ? strerror(errno) : "");
        if (n > 0) dump16(buf, (size_t)n);
    }

    /* ---- S3: pread 在若干物理地址 ---- */
    const uint64_t phys[] = { 0ULL, 0x100000ULL, 0x27800000ULL };
    for (unsigned i = 0; i < sizeof(phys)/sizeof(phys[0]); i++) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = pread(fd, buf, 16, (off_t)phys[i]);
        step("[S3] pread(phys=0x%09llx,16) -> %zd %s",
             (unsigned long long)phys[i], n, n < 0 ? strerror(errno) : "");
        if (n > 0) dump16(buf, (size_t)n);
    }

    /* ---- S4: 极少量 ioctl 只读探测 ---- */
    /* 只试 top-level magic+常见 nr 的几条, 绝不扫 26 万条 */
    const unsigned long cmds[] = { 0xc0304d00UL, 0xc0304d01UL, 0xc0304d02UL,
                                   0xc0304d03UL, 0x80044d00UL };
    for (unsigned i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++) {
        memset(buf, 0, sizeof(buf));
        errno = 0;
        int r = ioctl(fd, cmds[i], buf);
        step("[S4] ioctl(0x%08lx) -> %d %s", cmds[i], r, strerror(errno));
    }

    /* ---- S5: mmap offset=0 并读 ---- */
    step("[S5] 准备 mmap(offset=0, len=4096, PROT_READ, MAP_SHARED) —— 下一步若失联即为此步致命");
    void *m0 = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    step("[S5] mmap -> %p %s", m0, m0 == MAP_FAILED ? strerror(errno) : "");
    if (m0 != MAP_FAILED) {
        volatile unsigned char *p = (volatile unsigned char *)m0;
        unsigned char tmp[16];
        for (int i = 0; i < 16; i++) tmp[i] = p[i];
        step("[S5] 读到 16 字节:");
        dump16(tmp, 16);
        munmap(m0, 4096);
        step("[S5] munmap OK");
    }

    /* ---- S6: mmap 指定物理地址 ---- */
    uint64_t target = 0x27800000ULL;   /* recovery_fbaddr: cmdline 确认存在的真实 DRAM */
    step("[S6] 准备 mmap(phys=0x%09llx) —— 下一步若失联即为此步致命",
         (unsigned long long)target);
    uint64_t pg = target & ~(uint64_t)(ps - 1);
    void *m1 = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, (off_t)pg);
    step("[S6] mmap -> %p %s", m1, m1 == MAP_FAILED ? strerror(errno) : "");
    if (m1 != MAP_FAILED) {
        unsigned char tmp[16];
        volatile unsigned char *p = (volatile unsigned char *)m1 + (target - pg);
        for (int i = 0; i < 16; i++) tmp[i] = p[i];
        step("[S6] 读到 16 字节:");
        dump16(tmp, 16);
        munmap(m1, 4096);
        step("[S6] munmap OK");
    }

    close(fd);
    step("[END] miostep 全部步骤完成, 设备未挂");
    return 0;
}
