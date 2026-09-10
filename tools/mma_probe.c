/* mma_probe.c - 刻画 /dev/mma 设备(只读探测，不做危险 ioctl) */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>

int main(void) {
    struct stat st;
    const char *paths[] = {"/dev/mma", "/proc/mstar_dvfs", "/dev/diagnosis",
                           "/dev/mma0", "/dev/mma1", "/dev/mt_mma", NULL};
    for (int i = 0; paths[i]; i++) {
        if (stat(paths[i], &st) == 0) {
            printf("stat %-20s mode=%06o type=%s major=%d minor=%d size=%lld\n",
                   paths[i], st.st_mode,
                   S_ISCHR(st.st_mode) ? "char" : S_ISBLK(st.st_mode) ? "block" :
                   S_ISDIR(st.st_mode) ? "dir" : "other",
                   major(st.st_rdev), minor(st.st_rdev), (long long)st.st_size);
        } else {
            printf("stat %-20s ERR %s\n", paths[i], strerror(errno));
        }
    }
    printf("\n--- open /dev/mma and probe ioctl range (harmless queries) ---\n");
    int fd = open("/dev/mma", O_RDWR);
    if (fd < 0) { printf("open O_RDWR failed: %s\n", strerror(errno)); return 1; }
    printf("open O_RDWR -> fd=%d OK\n", fd);
    /* 常见"无害"ioctl：_IO(type, 0) —— 只带类型号不带参数，驱动通常返回 EINVAL/ENOTTY */
    for (int t = 0x40; t <= 0x7f && t <= 0x46; t++) {
        errno = 0;
        int r = ioctl(fd, (t << 8) | 0, 0);
        if (r != -1 || errno != ENOTTY)
            printf("  ioctl type=0x%02x nr=0 -> r=%d errno=%d(%s)\n", t, r, errno, strerror(errno));
    }
    close(fd);
    return 0;
}
