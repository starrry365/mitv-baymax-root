/* mmatest3.c — 扫 tag 名 / flags，找能成功 alloc 的组合 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define MMA_ALLOC 0xc0384d13u

static const char *names[] = {
    "AMM", "MMA", "mma", "MI_MMA", "MIMMA", "mmap", "ion", "ION",
    "graphic", "Graphic", "GRAPHIC", "disp", "DISP", "vdec", "VDEC",
    "osd", "OSD", "video", "VIDEO", "fb", "FB_MAIN", "GFX", "gfx",
    "system", "default", "VE", "ve", "VE_OSD", "snapshot", "JPEG", "dmabuf",
    NULL
};

int main(void) {
    int fd = open("/dev/mma", O_RDWR);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }
    printf("=== 扫 tag x size x flags ===\n");

    unsigned int sizes[] = {4096, 65536, 0x100000};
    for (int ni = 0; names[ni]; ni++) {
        for (int si = 0; si < 3; si++) {
            for (unsigned int fl = 0; fl <= 7; fl++) {
                unsigned char b[64]; memset(b, 0, sizeof b);
                strncpy((char *)b, names[ni], 15);
                uint32_t sz = sizes[si];
                memcpy(b + 0x20, &sz, 4);
                memcpy(b + 0x30, &fl, 4);
                errno = 0;
                int r = ioctl(fd, MMA_ALLOC, b);
                if (r >= 0) {
                    int ofd = -1; unsigned long long pa = 0;
                    memcpy(&ofd, b + 0x24, 4);
                    memcpy(&pa, b + 0x28, 8);
                    printf("*** 成功! name=%-10s size=%-8u flags=%u  r=%d fd=%d pa=0x%llx\n",
                           names[ni], sz, fl, r, ofd, pa);
                }
            }
        }
        printf("  [%-10s] 全尺寸/flags 均失败(errno=%d)\n", names[ni], errno);
    }
    /* 打印几种典型 errno */
    printf("\n=== 参考 ===");
    {
        unsigned char b[64]; memset(b, 0, sizeof b);
        strcpy((char *)b, "AMM"); uint32_t sz = 4096;
        memcpy(b + 0x20, &sz, 4);
        errno = 0; int r = ioctl(fd, MMA_ALLOC, b);
        printf(" name=AMM size=4096 flags=0 -> r=%d errno=%d(%s)\n", r, errno, strerror(errno));
    }
    close(fd);
    printf("ALLDONE\n");
    return 0;
}
