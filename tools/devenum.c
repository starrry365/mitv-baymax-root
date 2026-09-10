/* devenum.c - 枚举设备节点可访问性(open+fstat，不需要 ls 目录权限) */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>

static const char *cands[] = {
    "/dev/miomap", "/dev/malloc", "/dev/cmapool", "/dev/env_handler",
    "/dev/system", "/dev/mdlactl", "/dev/pmae", "/dev/fusion",
    "/dev/tee", "/dev/tee0", "/dev/teepriv0",
    "/dev/log2usb", "/dev/cli", "/dev/cb", "/dev/cb2", "/dev/mtal",
    "/dev/msmailbox", "/dev/semutex", "/dev/PM", "/dev/localdimming",
    "/dev/mstar_share_resource", "/dev/gpiochip0", "/dev/rpmb", "/dev/rtc0",
    "/dev/gflip", "/dev/mik", "/dev/ir", "/dev/smart", "/dev/XC",
    "/dev/ModIIC", "/dev/mtk_xtest", "/dev/fbm", "/dev/feeder", "/dev/mtphoto",
    "/dev/block/mmcblk0boot0", "/dev/block/mmcblk0rpmb",
    "/dev/ttyS0", "/dev/ttyS1", "/dev/ttyGS0", "/dev/ttyUSB0",
    "/dev/kmsg", "/dev/console", "/dev/ptmx", "/dev/zero",
    NULL
};

int main(void) {
    for (int i = 0; cands[i]; i++) {
        int r = open(cands[i], O_RDONLY);
        int re = errno;
        int w = open(cands[i], O_RDWR);
        int we = errno;
        if (r < 0 && w < 0) {
            /* 两个都失败就不打印(减少噪声)，除非不是 ENOENT */
            if (re != ENOENT) printf("%-32s [R:%s] [W:%s]\n", cands[i], strerror(re), strerror(we));
            continue;
        }
        struct stat st = {0};
        int fd = (w >= 0) ? w : r;
        fstat(fd, &st);
        printf("%-32s R=%-3s W=%-3s  char major=%d minor=%d\n",
               cands[i], r >= 0 ? "OK" : "no", w >= 0 ? "OK" : "no",
               major(st.st_rdev), minor(st.st_rdev));
        if (r >= 0) close(r);
        if (w >= 0) close(w);
    }
    printf("\n---- /proc/iomem 物理地址泄露测试 ----\n");
    int f = open("/proc/iomem", O_RDONLY);
    if (f >= 0) {
        char buf[512]; int n = read(f, buf, sizeof(buf)-1);
        if (n > 0) { buf[n] = 0; printf("%s\n", buf); }
        close(f);
    } else printf("open /proc/iomem: %s\n", strerror(errno));

    printf("\n---- /proc/self/maps (进程映射，可用于回推内核布局) ----\n");
    f = open("/proc/self/maps", O_RDONLY);
    if (f >= 0) { char b[300]; int n = read(f, b, sizeof(b)-1); if (n>0){b[n]=0; printf("%s\n", b);} close(f); }
    return 0;
}
