/* mmatest.c - 用还原出的真实 ioctl 码实测 /dev/mma 协议 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

static void dump(const char *tag, unsigned char *b, int n) {
    printf("    %s:", tag);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

static void t(int fd, uint32_t cmd, const char *name, int sz) {
    unsigned char buf[512];
    memset(buf, 0, sizeof buf);
    /* 对 alloc 类，填一个温和的 size（比如 4096）到 +0x04，避免零值 EINVAL */
    errno = 0;
    int r = ioctl(fd, cmd, buf);
    printf("[%#010x] %-24s -> r=%d errno=%d(%s)\n", cmd, name, r, errno, strerror(errno));
    if (r >= 0 || errno == EINVAL) dump("buf", buf, sz > 64 ? 64 : sz);
    return;
}

int main(void) {
    int fd = open("/dev/mma", O_RDWR);
    if (fd < 0) { printf("open /dev/mma: %s\n", strerror(errno)); return 1; }
    printf("fd=%d  /dev/mma 已打开\n\n", fd);

    printf("===== A. 只读查询类 ====\n");
    t(fd, 0xc0304d0b, "mma_get_meminfo", 48);
    t(fd, 0xc0304d0c, "mma_get_heapinfo", 48);
    t(fd, 0x80304d02, "mma_get_pipeid", 48);
    t(fd, 0xc01c4d0d, "mma_query_buf_tag", 28);
    t(fd, 0x80044d00, "MMA_?(_IOR 4B)", 4);
    t(fd, 0x80044d03, "MMA_?(0x4d03)", 4);
    t(fd, 0x80044d0b, "MMA_?(0x4d0b)", 4);
    t(fd, 0x80184d07, "dcache_flush_by_addr", 24);
    t(fd, 0x80184d09, "dcache_flush_all", 24);

    printf("\n===== B. 分配类(用 size=4096 探测) ====\n");
    {
        unsigned char buf[512];
        memset(buf, 0, sizeof buf);
        /* 常见布局: +0x00 handle/out, +0x04 size */
        *(uint32_t*)(buf + 0x04) = 4096;
        *(uint32_t*)(buf + 0x08) = 4096;
        errno = 0;
        int r = ioctl(fd, 0xc0304d03, buf);
        printf("[%#010x] %-24s -> r=%d errno=%d(%s)\n", 0xc0304d03, "mma_alloc_internal", r, errno, strerror(errno));
        dump("alloc buf", buf, 48);
        /* 若拿到句柄，尝试 get_meminfo/va2iova */
        if (r >= 0) {
            unsigned char q[512]; memset(q,0,sizeof q);
            memcpy(q, buf, 16);
            errno=0; int r2 = ioctl(fd, 0xc0304d0b, q);
            printf("  -> get_meminfo(handle): r=%d errno=%s\n", r2, r2<0?strerror(errno):"ok");
            dump("   info", q, 48);
        }
    }

    printf("\n===== C. 危险类(暂不调用, 仅列出) ====\n");
    printf("  0x40284d06 mma_map          (40B)\n");
    printf("  0x40284d05 mma_free         (40B)\n");
    printf("  0xc0284d09 mma_export_globalname (40B)\n");
    printf("  0xc0284d0a mma_import_globalname (40B)\n");
    printf("  0xc0a84d00 mma_reserve_iova (168B)\n");
    printf("  0xc0304d17 mma_va2iova      (48B)\n");
    close(fd);
    return 0;
}
