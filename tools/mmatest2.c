/* mmatest2.c — 基于真实实现（gralloc.mt5872.so）还原的 MMA 协议实测
 *
 * 已从反汇编确认：
 *   mma_open()            -> open("/dev/mma", O_RDWR)
 *   mma_get_pipeid(int*)  -> ioctl(fd, 0x80304d02, buf48)
 *   mma_alloc(name,size,&pa,&fd,flags) -> ioctl(fd, 0xc0384d13, buf56)
 *        buf+0x00 name[16], buf+0x20 = size(u32), buf+0x30 = flags(u32)
 *        out: buf+0x24 = fd(int), buf+0x28 = pa(u64)
 *   mma_get_meminfo(fd, *info)  -> ioctl(fd, 0xc0304d0b, buf48)
 *        in: buf+0x24 = fd ; out: buf+0x18..0x23 (12B) + buf+0x28 (1B)
 *   mma_map(fd, u32, u32) -> ioctl(fd, 0x40284d06, buf40)
 *   mma_free(fd)          -> ioctl(fd, 0x40284d05, buf40)
 *   mma_unmap(void*, u32) -> ioctl(fd, 0x40284d08?/0x40284d07?, ...)
 * 全局 fd（mma_open 返回的 /dev/mma 句柄）在这里自己 open 一次即可。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define MMA_ALLOC       0xc0384d13u   /* RW 'M' nr=0x13 size=56 */
#define MMA_GET_PIPEID  0x80304d02u   /* R  'M' nr=0x02 size=48 */
#define MMA_GET_MEMINFO 0xc0304d0bu   /* RW 'M' nr=0x0b size=48 */
#define MMA_GET_HEAPINFO 0xc0304d0cu  /* RW 'M' nr=0x0c size=48 */
#define MMA_MAP         0x40284d06u   /* W  'M' nr=0x06 size=40 */
#define MMA_FREE        0x40284d05u   /* W  'M' nr=0x05 size=40 */
#define MMA_EXPORT_GLB  0xc0284d09u   /* RW 'M' nr=0x09 size=40 */
#define MMA_IMPORT_GLB  0xc0284d0au   /* RW 'M' nr=0x0a size=40 */
#define MMA_QUERY_TAG   0xc01c4d0du   /* RW 'M' nr=0x0d size=28 */
#define MMA_VA2IOVA     0xc0304d17u   /* 待核 */
#define MMA_RSV_IOVA    0xc0a84d00u   /* RW 'M' nr=0x00 size=168 */

static void hexdump(const char *tag, const unsigned char *b, int n) {
    printf("    %s:", tag);
    for (int i = 0; i < n; i++) {
        if (i && i % 16 == 0) printf("\n        ");
        printf(" %02x", b[i]);
    }
    printf("\n");
}

static void u32dump(const char *tag, const unsigned char *b, int n) {
    printf("    %s:", tag);
    for (int i = 0; i + 4 <= n; i += 4) {
        uint32_t v; memcpy(&v, b + i, 4);
        printf(" [+%02x]=%08x", i, v);
    }
    printf("\n");
    printf("    %s (u64):", tag);
    for (int i = 0; i + 8 <= n; i += 8) {
        uint64_t v; memcpy(&v, b + i, 8);
        printf(" [+%02x]=%016llx", i, (unsigned long long)v);
    }
    printf("\n");
}

int main(void) {
    int fd = open("/dev/mma", O_RDWR);
    if (fd < 0) { printf("open /dev/mma: %s\n", strerror(errno)); return 1; }
    printf("=== /dev/mma fd=%d ===\n", fd);

    /* ---------- 1. get_pipeid ---------- */
    printf("\n[1] mma_get_pipeid  0x%08x\n", MMA_GET_PIPEID);
    unsigned char p[64]; memset(p, 0, sizeof p);
    errno = 0;
    int r = ioctl(fd, MMA_GET_PIPEID, p);
    printf("    r=%d errno=%d(%s)\n", r, errno, strerror(errno));
    if (r >= 0) u32dump("pipeid", p, 48);

    /* ---------- 2. get_meminfo 探参数 ---------- */
    printf("\n[2] mma_get_meminfo 0x%08x (fd=0 试探)\n", MMA_GET_MEMINFO);
    unsigned char mi[64]; memset(mi, 0, sizeof mi);
    errno = 0; r = ioctl(fd, MMA_GET_MEMINFO, mi);
    printf("    r=%d errno=%d(%s)\n", r, errno, strerror(errno));
    if (r >= 0) u32dump("meminfo", mi, 48);

    /* ---------- 3. alloc ---------- */
    printf("\n[3] mma_alloc 0x%08x\n", MMA_ALLOC);
    unsigned char a[64]; memset(a, 0, sizeof a);
    strcpy((char *)a, "prober");           /* +0x00 name[16] */
    uint32_t size = 4096;
    memcpy(a + 0x20, &size, 4);            /* +0x20 size */
    uint32_t flags = 0;
    memcpy(a + 0x30, &flags, 4);           /* +0x30 flags */
    errno = 0; r = ioctl(fd, MMA_ALLOC, a);
    printf("    size=%u flags=%u -> r=%d errno=%d(%s)\n", size, flags, r, errno, strerror(errno));
    u32dump("alloc", a, 56);

    int  got_fd = -1; unsigned long long got_pa = 0; int ok = 0;
    if (r >= 0) {
        memcpy(&got_fd, a + 0x24, 4);
        memcpy(&got_pa, a + 0x28, 8);
        printf("    >>> alloc 成功: fd=%d pa=0x%llx\n", got_fd, got_pa);
        ok = 1;
    } else {
        /* 试更大 size */
        for (unsigned int s2 = 0x10000; s2 <= 0x1000000; s2 <<= 2) {
            memset(a, 0, sizeof a);
            strcpy((char *)a, "prober");
            memcpy(a + 0x20, &s2, 4);
            errno = 0; r = ioctl(fd, MMA_ALLOC, a);
            memcpy(&got_fd, a + 0x24, 4);
            memcpy(&got_pa, a + 0x28, 8);
            printf("    retry size=0x%x -> r=%d errno=%d(%s) fd=%d pa=0x%llx\n",
                   s2, r, errno, strerror(errno), got_fd, got_pa);
            if (r >= 0) { ok = 1; break; }
        }
    }

    /* ---------- 4. 用拿到的 fd 反查物理地址 ---------- */
    if (ok && got_fd >= 0) {
        printf("\n[4] mma_get_meminfo(fd=%d) 反查 pa\n", got_fd);
        unsigned char mi2[64]; memset(mi2, 0, sizeof mi2);
        memcpy(mi2 + 0x24, &got_fd, 4);
        errno = 0; r = ioctl(fd, MMA_GET_MEMINFO, mi2);
        printf("    r=%d errno=%d(%s)\n", r, errno, strerror(errno));
        u32dump("meminfo(fd)", mi2, 48);
    }

    /* ---------- 5. quote_buf_tag ---------- */
    printf("\n[5] mma_query_buf_tag 0x%08x\n", MMA_QUERY_TAG);
    unsigned char q[32]; memset(q, 0, sizeof q);
    strcpy((char *)q, "prober");
    errno = 0; r = ioctl(fd, MMA_QUERY_TAG, q);
    printf("    r=%d errno=%d(%s)\n", r, errno, strerror(errno));
    if (r >= 0) u32dump("buf_tag", q, 28);

    /* ---------- 6. get_heapinfo ---------- */
    printf("\n[6] mma_get_heapinfo 0x%08x\n", MMA_GET_HEAPINFO);
    unsigned char h[64]; memset(h, 0, sizeof h);
    strcpy((char *)h, "prober");
    errno = 0; r = ioctl(fd, MMA_GET_HEAPINFO, h);
    printf("    r=%d errno=%d(%s)\n", r, errno, strerror(errno));
    if (r >= 0) u32dump("heapinfo", h, 48);

    /* ---------- 7. 危险动作：先只打印，不执行 ---------- */
    printf("\n[7] 尚未调用(需确认布局后手动放开):\n");
    printf("    mma_map     0x%08x  (fd=%d)\n", MMA_MAP, got_fd);
    printf("    mma_free    0x%08x  (fd=%d)\n", MMA_FREE, got_fd);
    printf("    mma_export_globalname 0x%08x\n", MMA_EXPORT_GLB);
    printf("    mma_import_globalname 0x%08x\n", MMA_IMPORT_GLB);
    printf("    mma_reserve_iova_space 0x%08x\n", MMA_RSV_IOVA);

    close(fd);
    printf("\nALLDONE\n");
    return 0;
}
